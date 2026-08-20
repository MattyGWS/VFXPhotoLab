#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="${OCIO_ROOT:-$ROOT_DIR/third_party/opencolorio}"
CONFIG="$PREFIX/lib/cmake/OpenColorIO/OpenColorIOConfig.cmake"
CONFIG64="$PREFIX/lib64/cmake/OpenColorIO/OpenColorIOConfig.cmake"
VERSION="2.5.2"
ARCHIVE="$ROOT_DIR/build/deps/downloads/opencolorio-${VERSION}.tar.gz"
SOURCE="$ROOT_DIR/build/deps/src/opencolorio-${VERSION}"
BUILD="$ROOT_DIR/build/deps/opencolorio-${VERSION}"
URL="https://files.pythonhosted.org/packages/6a/ea/9d930df6740f9b09b0b342f40a5ef165da5050141e496081ef80b302e566/opencolorio-${VERSION}.tar.gz"
SHA256="fecebd0914089b0c8238c55648f8eb2ccd2702ab4b2eea53856a0e368ded8262"

prune_dependency_intermediates() {
    # The installed OCIO prefix and ext/dist are sufficient for all future
    # VFX Photo Lab links.  The extracted source and ExternalProject object
    # trees are disposable and can otherwise consume hundreds of megabytes.
    rm -rf "$SOURCE" "$BUILD/ext/build"
    if [[ -d "$BUILD" ]]; then
        find "$BUILD" -mindepth 1 -maxdepth 1 ! -name ext -exec rm -rf {} +
    fi
    if [[ -d "$BUILD/ext" ]]; then
        find "$BUILD/ext" -mindepth 1 -maxdepth 1 ! -name dist -exec rm -rf {} +
    fi
}

if [[ -f "$CONFIG" || -f "$CONFIG64" ]]; then
    if [[ -d "$BUILD/ext/dist" ]]; then
        prune_dependency_intermediates
        echo "OpenColorIO ${VERSION} is already present at $PREFIX"
        exit 0
    fi
    echo "OpenColorIO is installed, but its static dependency prefix is missing; rebuilding the dependency bundle."
fi

for command in cmake ninja python3; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "OpenColorIO acquisition requires $command." >&2
        exit 1
    }
done

# Prefer a suitable distribution-provided OpenColorIO before building a local
# copy. The main application configure will perform the same normal CMake
# package search after this probe succeeds.
SYSTEM_PROBE="$ROOT_DIR/build/deps/probe-opencolorio"
mkdir -p "$SYSTEM_PROBE/source"
cat > "$SYSTEM_PROBE/source/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.21)
project(VFXPhotoLabOcioProbe LANGUAGES CXX)
find_package(OpenColorIO 2.5.2 CONFIG REQUIRED)
CMAKE
if cmake -S "$SYSTEM_PROBE/source" -B "$SYSTEM_PROBE/build" -G Ninja \
         -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1; then
    echo "OpenColorIO ${VERSION} is available from the system CMake package path."
    exit 0
fi

mkdir -p "$(dirname "$ARCHIVE")" "$(dirname "$SOURCE")" "$PREFIX"
if [[ ! -f "$ARCHIVE" ]]; then
    echo "Downloading OpenColorIO ${VERSION} source package..."
    python3 - "$URL" "$ARCHIVE" <<'PY'
import pathlib, sys, urllib.request
url, destination = sys.argv[1], pathlib.Path(sys.argv[2])
temporary = destination.with_suffix(destination.suffix + '.part')
with urllib.request.urlopen(url, timeout=120) as response, temporary.open('wb') as output:
    while True:
        block = response.read(1024 * 1024)
        if not block:
            break
        output.write(block)
temporary.replace(destination)
PY
fi

actual="$(python3 - "$ARCHIVE" <<'PY'
import hashlib, pathlib, sys
h=hashlib.sha256()
with pathlib.Path(sys.argv[1]).open('rb') as f:
    for chunk in iter(lambda: f.read(1024*1024), b''):
        h.update(chunk)
print(h.hexdigest())
PY
)"
if [[ "$actual" != "$SHA256" ]]; then
    echo "OpenColorIO archive checksum mismatch." >&2
    rm -f "$ARCHIVE"
    exit 1
fi

if [[ ! -f "$SOURCE/CMakeLists.txt" ]]; then
    rm -rf "$SOURCE"
    mkdir -p "$SOURCE"
    python3 - "$ARCHIVE" "$SOURCE" <<'PY'
import pathlib, tarfile, sys
archive, destination = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
with tarfile.open(archive, 'r:gz') as tf:
    members = tf.getmembers()
    roots = {m.name.split('/', 1)[0] for m in members if m.name}
    prefix = next(iter(roots)) + '/' if len(roots) == 1 else ''
    for member in members:
        name = member.name[len(prefix):] if prefix and member.name.startswith(prefix) else member.name
        if not name:
            continue
        target = (destination / name).resolve()
        if destination.resolve() not in target.parents and target != destination.resolve():
            raise RuntimeError('Unsafe path in OpenColorIO source archive')
        member.name = name
        try:
            tf.extract(member, destination, filter='data')
        except TypeError:
            # Python 3.11 and older do not expose extraction filters. The
            # explicit resolved-path check above still prevents traversal.
            tf.extract(member, destination)
PY
fi

# yaml-cpp 0.8.0, which is the dependency pinned by OpenColorIO 2.5.2,
# relied on transitive fixed-width integer declarations. GCC 16 no longer
# exposes those declarations through the same incidental include chain. Patch
# OCIO's private ExternalProject arguments so only its vendored yaml-cpp build
# force-includes the required standard header. This does not modify VFX Photo
# Lab's compiler flags or any system installation.
python3 - "$SOURCE/share/cmake/modules/install/Installyaml-cpp.cmake" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
marker = "VFX Photo Lab GCC 16 yaml-cpp compatibility"
if marker not in text:
    needle = '        string(STRIP "${yaml-cpp_CXX_FLAGS}" yaml-cpp_CXX_FLAGS)'
    if needle not in text:
        raise RuntimeError('OpenColorIO yaml-cpp install module has an unexpected layout')
    patch = '''        # VFX Photo Lab GCC 16 yaml-cpp compatibility.
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
           AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16)
            string(APPEND yaml-cpp_CXX_FLAGS " -include cstdint")
        endif()

'''
    path.write_text(text.replace(needle, patch + needle, 1))
PY

# If this project is being overlaid onto a folder where the original 0.11.0e
# bootstrap already failed, patch that checked-out yaml-cpp source too. This
# allows Ninja to resume instead of rebuilding all completed dependencies.
python3 - "$BUILD/ext/build/yaml-cpp/src/yaml-cpp_install/src/emitterutils.cpp" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
if path.is_file():
    text = path.read_text()
    if '#include <cstdint>' not in text:
        first_include = text.find('#include')
        if first_include < 0:
            raise RuntimeError('Existing yaml-cpp emitter source has an unexpected layout')
        path.write_text(text[:first_include] + '#include <cstdint>\n' + text[first_include:])
PY

mkdir -p "$BUILD"

OCIO_PRIVATE_CXX_FLAGS=""
if [[ "$(c++ -dumpversion 2>/dev/null || true)" =~ ^([0-9]+) ]] \
   && (( BASH_REMATCH[1] >= 16 )); then
    # GCC 16 emits an inlining-based array-bounds diagnostic in OCIO 2.5.2's
    # CDLOp shared_ptr teardown.  It does not fail the build or affect the
    # generated library, so suppress it only for this vendored dependency.
    OCIO_PRIVATE_CXX_FLAGS="-Wno-array-bounds"
fi

cmake -S "$SOURCE" -B "$BUILD" -G Ninja -Wno-dev \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_CXX_FLAGS="$OCIO_PRIVATE_CXX_FLAGS" \
    -DBUILD_SHARED_LIBS=OFF \
    -DOCIO_BUILD_APPS=OFF \
    -DOCIO_BUILD_DOCS=OFF \
    -DOCIO_BUILD_GPU_TESTS=OFF \
    -DOCIO_BUILD_PYTHON=OFF \
    -DOCIO_BUILD_TESTS=OFF \
    -DOCIO_INSTALL_EXT_PACKAGES=MISSING
cmake --build "$BUILD" --parallel
cmake --install "$BUILD"
prune_dependency_intermediates

echo "Installed OpenColorIO ${VERSION} to $PREFIX"
echo "Pruned disposable OpenColorIO source and object intermediates; retained the installed package and static dependency bundle."
