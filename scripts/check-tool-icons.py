#!/usr/bin/env python3
"""Validate the toolbar's one-tool/one-icon resource contract."""

from __future__ import annotations

import re
import struct
import sys
from collections import Counter
from pathlib import Path
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "src" / "MainWindow.cpp"
QRC = ROOT / "resources" / "resources.qrc"
ICON_DIR = ROOT / "resources" / "icons"
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def fail(messages: list[str]) -> int:
    for message in messages:
        print(f"Tool icon validation error: {message}", file=sys.stderr)
    return 1


def png_header(path: Path) -> tuple[int, int, int, int]:
    with path.open("rb") as stream:
        if stream.read(8) != PNG_SIGNATURE:
            raise ValueError("not a PNG file")
        length = struct.unpack(">I", stream.read(4))[0]
        chunk_type = stream.read(4)
        if chunk_type != b"IHDR" or length != 13:
            raise ValueError("missing standard IHDR chunk")
        width, height, bit_depth, colour_type = struct.unpack(">IIBB", stream.read(10))
        return width, height, bit_depth, colour_type


def main() -> int:
    errors: list[str] = []
    source = SOURCE.read_text(encoding="utf-8")
    start_marker = "void MainWindow::buildToolsToolbar()"
    end_marker = "void MainWindow::buildColorDock()"
    try:
        start = source.index(start_marker)
        end = source.index(end_marker, start)
    except ValueError:
        return fail(["could not locate buildToolsToolbar() source block"])

    paths = re.findall(
        r'QStringLiteral\(\":/icons/([^\"]+\.png)\"\)',
        source[start:end],
    )
    if not paths:
        errors.append("no toolbar icon resources were found")

    duplicates = {name: count for name, count in Counter(paths).items() if count > 1}
    for name, count in sorted(duplicates.items()):
        errors.append(f"{name} is claimed by {count} tools; each tool needs its own file")

    try:
        tree = ET.parse(QRC)
        qrc_paths = {
            element.text
            for element in tree.findall(".//file")
            if element.text and element.text.startswith("icons/")
        }
    except (ET.ParseError, OSError) as error:
        return fail([f"cannot read resources.qrc: {error}"])

    for name in paths:
        path = ICON_DIR / name
        if not path.is_file():
            errors.append(f"missing resources/icons/{name}")
            continue
        if f"icons/{name}" not in qrc_paths:
            errors.append(f"icons/{name} is not compiled by resources.qrc")
        try:
            width, height, bit_depth, colour_type = png_header(path)
        except (OSError, ValueError, struct.error) as error:
            errors.append(f"{name}: {error}")
            continue
        if (width, height) != (24, 24):
            errors.append(f"{name} is {width}x{height}; expected 24x24")
        if bit_depth != 8 or colour_type != 6:
            errors.append(
                f"{name} must be 8-bit RGBA PNG (bit depth 8, colour type 6); "
                f"found bit depth {bit_depth}, colour type {colour_type}"
            )

    if errors:
        return fail(errors)

    print(f"Tool icon validation passed ({len(paths)} unique 24x24 RGBA resources).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
