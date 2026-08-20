#!/usr/bin/env python3
"""Prepare, validate, and package VFX Photo Lab Windows releases."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile
from dataclasses import asdict, dataclass
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
CMAKE_LISTS = PROJECT_ROOT / "CMakeLists.txt"
CHANGELOG = PROJECT_ROOT / "CHANGELOG.md"
APP_DIRECTORY_NAME = "VFX Photo Lab"
APP_EXE_NAME = "VFXPhotoLab.exe"


@dataclass(frozen=True, slots=True)
class ReleaseMetadata:
    version: str
    windows_version: str
    tag: str
    asset_stem: str
    portable_filename: str
    installer_filename: str
    checksum_filename: str
    artifact_name: str


def read_project_version() -> str:
    text = CMAKE_LISTS.read_text(encoding="utf-8")
    match = re.search(
        r'set\s*\(\s*VFXPHOTOLAB_RELEASE_VERSION\s+"([^"]+)"\s*\)',
        text,
        re.IGNORECASE | re.MULTILINE,
    )
    if not match:
        raise RuntimeError(
            "Could not find set(VFXPHOTOLAB_RELEASE_VERSION \"...\") in CMakeLists.txt."
        )
    version = match.group(1).strip()
    if not re.fullmatch(r"[0-9A-Za-z][0-9A-Za-z._-]*", version):
        raise RuntimeError(f"Unsupported release version syntax: {version!r}")
    if 'VFXPHOTOLAB_VERSION="${VFXPHOTOLAB_RELEASE_VERSION}"' not in text:
        raise RuntimeError(
            "CMakeLists.txt must compile VFXPHOTOLAB_VERSION from "
            "VFXPHOTOLAB_RELEASE_VERSION so release metadata and About cannot drift."
        )
    return version


def numeric_version(version: str) -> tuple[int, int, int, int]:
    values = [int(value) for value in re.findall(r"\d+", version)[:4]]
    padded = (values + [0, 0, 0, 0])[:4]
    if any(value > 65535 for value in padded):
        raise RuntimeError(f"Windows version components must be <= 65535: {version}")
    return tuple(padded)  # type: ignore[return-value]


def metadata() -> ReleaseMetadata:
    version = read_project_version()
    stem = f"VFXPhotoLab-{version}-Windows-x64"
    win_version = ".".join(str(value) for value in numeric_version(version))
    return ReleaseMetadata(
        version=version,
        windows_version=win_version,
        tag=f"v{version}",
        asset_stem=stem,
        portable_filename=f"{stem}-Portable.zip",
        installer_filename=f"{stem}-Setup.exe",
        checksum_filename=f"{stem}-SHA256.txt",
        artifact_name=stem,
    )


def changelog_notes(version: str) -> str:
    text = CHANGELOG.read_text(encoding="utf-8")
    pattern = re.compile(
        rf"^##\s+{re.escape(version)}(?P<title>[^\n]*)\n(?P<body>.*?)(?=^##\s+|\Z)",
        re.MULTILINE | re.DOTALL,
    )
    match = pattern.search(text)
    if not match:
        raise RuntimeError(
            f"CHANGELOG.md has no '## {version}' release section. "
            "Add the patch notes before publishing."
        )
    title = f"{version}{match.group('title')}".strip()
    body = match.group("body").strip()
    if not body:
        raise RuntimeError(f"CHANGELOG.md section for {version} is empty.")
    return f"# {title}\n\n{body}\n"


def write_github_outputs(path: Path, values: dict[str, str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8", newline="\n") as handle:
        for key, value in values.items():
            handle.write(f"{key}={value}\n")


def command_prepare(args: argparse.Namespace) -> int:
    info = metadata()
    notes_path = Path(args.release_notes)
    notes_path.parent.mkdir(parents=True, exist_ok=True)
    notes_path.write_text(changelog_notes(info.version), encoding="utf-8")
    metadata_path = Path(args.metadata_json)
    metadata_path.parent.mkdir(parents=True, exist_ok=True)
    metadata_path.write_text(json.dumps(asdict(info), indent=2) + "\n", encoding="utf-8")
    if args.github_output:
        write_github_outputs(Path(args.github_output), asdict(info))
    print(json.dumps(asdict(info), indent=2))
    return 0


def _all_files(root: Path) -> list[Path]:
    return sorted(path for path in root.rglob("*") if path.is_file())


def _relative_lower(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix().lower()


def _has_any_relative(root: Path, candidates: tuple[str, ...]) -> bool:
    wanted = {candidate.lower().replace("\\", "/") for candidate in candidates}
    return any(_relative_lower(path, root) in wanted for path in _all_files(root))


def command_verify(args: argparse.Namespace) -> int:
    dist = Path(args.dist).resolve()
    exe = dist / APP_EXE_NAME
    failures: list[str] = []
    if not exe.is_file():
        failures.append(f"Missing executable: {exe}")

    required_groups: tuple[tuple[str, ...], ...] = (
        ("Qt6Core.dll",),
        ("Qt6Gui.dll",),
        ("Qt6Widgets.dll",),
        ("Qt6Concurrent.dll",),
        ("platforms/qwindows.dll", "plugins/platforms/qwindows.dll"),
        ("imageformats/qjpeg.dll", "plugins/imageformats/qjpeg.dll"),
        ("LICENSE",),
        ("README.md",),
        ("CHANGELOG.md",),
        ("shaders/adjustment_tile.wgsl",),
        ("shaders/composite_tile.wgsl",),
    )
    for group in required_groups:
        if not _has_any_relative(dist, group):
            failures.append("Missing required packaged file: " + " or ".join(group))

    all_files = _all_files(dist) if dist.is_dir() else []
    wgpu_dlls = [
        path
        for path in all_files
        if path.suffix.lower() == ".dll" and "wgpu" in path.name.lower()
    ]
    if not wgpu_dlls:
        failures.append("No wgpu-native DLL was found in the portable tree.")

    if failures:
        for failure in failures:
            print(f"ERROR: {failure}", file=sys.stderr)
        return 1

    smoke_json = Path(args.smoke_json).resolve()
    smoke_json.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [
            str(exe),
            "--package-smoke-test",
            "--require-full-release",
            "--json",
            str(smoke_json),
        ],
        cwd=dist,
        timeout=args.timeout,
        check=False,
    )
    if result.returncode != 0:
        print(
            f"ERROR: Packaged executable smoke test returned {result.returncode}.",
            file=sys.stderr,
        )
        if smoke_json.is_file():
            print(smoke_json.read_text(encoding="utf-8"), file=sys.stderr)
        return result.returncode or 1
    if not smoke_json.is_file():
        print("ERROR: Packaged executable did not write its smoke-test report.", file=sys.stderr)
        return 1
    report = json.loads(smoke_json.read_text(encoding="utf-8"))
    if not report.get("ok"):
        print(json.dumps(report, indent=2), file=sys.stderr)
        return 1
    if report.get("version") != metadata().version:
        print(
            f"ERROR: Packaged executable reports version {report.get('version')!r}, "
            f"expected {metadata().version!r}.",
            file=sys.stderr,
        )
        return 1
    print(json.dumps(report, indent=2))
    return 0


def _zip_directory(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(
        destination, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
    ) as archive:
        for path in sorted(source.rglob("*")):
            if path.is_file():
                archive.write(path, Path(APP_DIRECTORY_NAME) / path.relative_to(source))


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def command_package(args: argparse.Namespace) -> int:
    info = metadata()
    dist = Path(args.dist).resolve()
    installer = Path(args.installer).resolve()
    release_dir = Path(args.release_dir).resolve()
    release_dir.mkdir(parents=True, exist_ok=True)
    if not (dist / APP_EXE_NAME).is_file():
        raise FileNotFoundError(f"Portable application directory is incomplete: {dist}")
    if not installer.is_file():
        raise FileNotFoundError(f"Inno Setup did not produce the installer: {installer}")

    portable = release_dir / info.portable_filename
    target_installer = release_dir / info.installer_filename
    if installer != target_installer:
        shutil.copy2(installer, target_installer)
    _zip_directory(dist, portable)

    checksum_path = release_dir / info.checksum_filename
    assets = (portable, target_installer)
    checksum_path.write_text(
        "".join(f"{_sha256(path)}  {path.name}\n" for path in assets),
        encoding="utf-8",
    )
    print(json.dumps({"assets": [str(path) for path in (*assets, checksum_path)]}, indent=2))
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    metadata_parser = subparsers.add_parser("metadata", help="Print release metadata as JSON.")
    metadata_parser.set_defaults(
        func=lambda _args: (print(json.dumps(asdict(metadata()), indent=2)) or 0)
    )

    prepare = subparsers.add_parser("prepare", help="Generate release notes and metadata.")
    prepare.add_argument("--release-notes", required=True)
    prepare.add_argument("--metadata-json", required=True)
    prepare.add_argument("--github-output")
    prepare.set_defaults(func=command_prepare)

    verify = subparsers.add_parser("verify", help="Validate and run the packaged application.")
    verify.add_argument("--dist", required=True)
    verify.add_argument("--smoke-json", required=True)
    verify.add_argument("--timeout", type=int, default=180)
    verify.set_defaults(func=command_verify)

    package = subparsers.add_parser("package", help="Create portable ZIP and checksums.")
    package.add_argument("--dist", required=True)
    package.add_argument("--installer", required=True)
    package.add_argument("--release-dir", required=True)
    package.set_defaults(func=command_package)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
