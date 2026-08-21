# Automated Windows builds and releases

VFX Photo Lab uses the same maintainer-facing release model as VFX Texture Lab: development can stay on Linux while GitHub Actions performs the clean Windows x64 build, packaging and installer validation.

## Release outputs

Every successful Windows release workflow produces:

- `VFXPhotoLab-<version>-Windows-x64-Portable.zip`
- `VFXPhotoLab-<version>-Windows-x64-Setup.exe`
- `VFXPhotoLab-<version>-Windows-x64-SHA256.txt`
- portable and installed-application smoke-test reports as GitHub Actions artifacts

The installer is per-user by default, creates normal Start Menu/uninstall entries, offers an optional desktop shortcut and associates `.vfxphoto` projects with VFX Photo Lab.

The Windows package is a full release build. The workflow requires both pinned wgpu-native and OpenColorIO support; it does not silently publish an ICC-only or CPU-only fallback build if either release dependency is missing.

## One-command maintainer release from Fedora/Linux

The normal release command is:

```bash
./tools/publish_windows_release.sh --yes
```

Before running it:

1. Set the new value of `VFXPHOTOLAB_RELEASE_VERSION` in `CMakeLists.txt`.
2. Add a matching `## <version>` section to `CHANGELOG.md` containing the release/patch notes.
3. Make sure the local checkout is on `main` and points at the `MattyGWS/VFXPhotoLab` GitHub repository.

The script then:

1. validates the release version and matching changelog section;
2. shows/collects every pending tracked and untracked source change;
3. commits them as `Release <version>`;
4. pushes `main`;
5. dispatches `.github/workflows/windows-release.yml` in `draft-release` mode;
6. waits for the clean Windows build, tests, deployment and installer smoke tests;
7. checks that the draft release contains the portable ZIP, installer and SHA-256 file;
8. publishes the draft as the latest GitHub Release.

Without `--yes`, the script asks once before committing and once before publishing. Use `--no-browser` if the release page should not be opened at the end.

### One-time local requirements

The publish script only needs ordinary Linux tools plus GitHub CLI:

```bash
git --version
gh --version
python3 --version
gh auth login
```

No Windows compiler, Qt SDK, Inno Setup, wgpu-native SDK or OpenColorIO build is required on the maintainer's Linux machine.

## Test a Windows build without making a release

A packaging test can be run entirely from GitHub:

1. Push the desired commit.
2. Open **Actions → Build Windows app**.
3. Choose **Run workflow**.
4. Leave **test-build** selected.
5. Download the `VFXPhotoLab-<version>-Windows-x64` artifact when the workflow completes.

No tag or GitHub Release is created in `test-build` mode.

## What GitHub validates

The Windows release job runs on Windows Server 2022 with MSVC and Qt 6.8 and performs all of the following before a release asset is accepted:

- WGSL reserved-word/source validation;
- tool-icon validation;
- pinned wgpu-native acquisition;
- pinned OpenColorIO 2.5.2 acquisition/build;
- Release CMake configure with native WebGPU and OpenColorIO required;
- the complete CTest suite;
- CMake/Qt runtime deployment into the portable application tree;
- presence of Qt Core/Gui/Widgets/Concurrent DLLs;
- presence of the Windows Qt platform and JPEG image-format plugins;
- presence of the wgpu-native runtime DLL;
- presence of the packaged shaders and project documentation/license files;
- a non-interactive `VFXPhotoLab.exe --package-smoke-test --require-full-release` check which verifies the executable version, image plugins, compiled OpenColorIO support and full WebGPU release marker without requiring a usable GPU on the CI runner;
- silent Inno Setup installation to a clean temporary directory;
- the same smoke test from the installed application;
- silent uninstall;
- portable ZIP creation and SHA-256 generation.

If any of these checks fail, the workflow fails before the local publish script can make the draft public.

## Versioning

`VFXPHOTOLAB_RELEASE_VERSION` in `CMakeLists.txt` is the authoritative public version string used by the application, Windows file metadata and release tooling.

CMake's numeric `project(VERSION ...)` remains separate because CMake cannot represent milestone suffixes such as `0.14.0m.2`. The release tooling converts the first four numeric components into Windows' numeric file version, for example:

- public version: `0.14.0m.2`
- Windows numeric version: `0.14.0.2`
- Git tag: `v0.14.0m.2`

Release notes are taken exclusively from the matching `## <version>` section of `CHANGELOG.md`.

## Local Windows build, when diagnosing packaging

The official release path is GitHub Actions. On a Windows development machine with Qt/MSVC/CMake/Ninja available, the existing build bootstrap remains useful:

```powershell
.\scripts\build-windows.ps1
```

For an exact release-package diagnosis, follow the configure/install commands in `.github/workflows/windows-release.yml`; that workflow is the authoritative packaging recipe.

## Signing

The current Windows installer and executable are unsigned. Microsoft Defender SmartScreen may therefore warn until the project has an established signing reputation. Code signing can be added later without changing the release command or package layout.


## Windows CI test diagnostics

Windows test runners are deliberately console-subsystem executables even though the shipped application is a GUI-subsystem executable. This preserves QtTest assertion output in GitHub Actions. The release workflow also writes `build/windows-release/ctest-results.xml` and includes it, together with `LastTest.log`, in the always-uploaded `Windows-build-diagnostics-*` artifact.
