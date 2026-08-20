#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BUILD_PRESET="${VFXPHOTOLAB_CMAKE_PRESET:-release}"

missing=()
command -v cmake >/dev/null 2>&1 || missing+=(cmake)
command -v ninja >/dev/null 2>&1 || missing+=(ninja-build)
command -v c++ >/dev/null 2>&1 || missing+=(gcc-c++)
command -v python3 >/dev/null 2>&1 || missing+=(python3)

if (( ${#missing[@]} > 0 )); then
    echo "Missing build tools: ${missing[*]}" >&2
    echo >&2
    echo "On Fedora, install the required dependencies with:" >&2
    echo "  sudo dnf install gcc-c++ cmake ninja-build python3 qt6-qtbase-devel qt6-qtimageformats" >&2
    exit 1
fi

"$ROOT_DIR/scripts/check-wgsl-reserved.py"
"$ROOT_DIR/scripts/check-tool-icons.py"

WEBGPU_CMAKE_ARG="-DVFXPHOTOLAB_ENABLE_WEBGPU=ON"
if [[ "${VFXPHOTOLAB_SKIP_WGPU_FETCH:-0}" == "1" ]]; then
    WEBGPU_CMAKE_ARG="-DVFXPHOTOLAB_ENABLE_WEBGPU=OFF"
    echo "Skipping wgpu-native acquisition; building with the CPU renderer only."
elif [[ -n "${WGPU_ROOT:-}" ]]; then
    echo "Using manually supplied wgpu-native SDK from WGPU_ROOT=$WGPU_ROOT"
elif ! "$ROOT_DIR/scripts/fetch-wgpu-native.sh"; then
    WEBGPU_CMAKE_ARG="-DVFXPHOTOLAB_ENABLE_WEBGPU=OFF"
    echo >&2
    echo "Warning: wgpu-native acquisition failed; continuing with a CPU-only build." >&2
fi

OCIO_CMAKE_ARGS=("-DVFXPHOTOLAB_ENABLE_OCIO=ON")
if [[ "${VFXPHOTOLAB_SKIP_OCIO_FETCH:-0}" == "1" ]]; then
    OCIO_CMAKE_ARGS=("-DVFXPHOTOLAB_ENABLE_OCIO=OFF")
    echo "Skipping OpenColorIO acquisition; building with ICC-only colour management."
else
    OCIO_PREFIX="${OCIO_ROOT:-$ROOT_DIR/third_party/opencolorio}"
    OCIO_DEPENDENCY_PREFIX="$ROOT_DIR/build/deps/opencolorio-2.5.2/ext/dist"

    if [[ -n "${OCIO_ROOT:-}" ]]; then
        echo "Using manually supplied OpenColorIO installation from OCIO_ROOT=$OCIO_ROOT"
    elif ! "$ROOT_DIR/scripts/fetch-opencolorio.sh"; then
        OCIO_CMAKE_ARGS=("-DVFXPHOTOLAB_ENABLE_OCIO=OFF")
        echo >&2
        echo "Warning: OpenColorIO acquisition failed; continuing with ICC-only colour management." >&2
    fi

    if [[ "${OCIO_CMAKE_ARGS[0]}" == "-DVFXPHOTOLAB_ENABLE_OCIO=ON" ]]; then
        OCIO_CONFIG_DIR=""
        for candidate in \
            "$OCIO_PREFIX/lib/cmake/OpenColorIO" \
            "$OCIO_PREFIX/lib64/cmake/OpenColorIO"; do
            if [[ -f "$candidate/OpenColorIOConfig.cmake" ]]; then
                OCIO_CONFIG_DIR="$candidate"
                break
            fi
        done

        OCIO_CMAKE_ARGS+=(
            "-DVFXPHOTOLAB_OCIO_ROOT=$OCIO_PREFIX"
            "-DVFXPHOTOLAB_OCIO_DEPENDENCY_ROOT=$OCIO_DEPENDENCY_PREFIX"
        )
        if [[ -n "$OCIO_CONFIG_DIR" ]]; then
            OCIO_CMAKE_ARGS+=("-DOpenColorIO_DIR=$OCIO_CONFIG_DIR")
            echo "Using OpenColorIO package: $OCIO_CONFIG_DIR"
            if [[ -d "$OCIO_DEPENDENCY_PREFIX" ]]; then
                echo "Using OpenColorIO static dependency prefix: $OCIO_DEPENDENCY_PREFIX"
            fi
        fi
    fi
fi

cmake --preset "$BUILD_PRESET" "$WEBGPU_CMAKE_ARG" "${OCIO_CMAKE_ARGS[@]}"
cmake --build --preset "$BUILD_PRESET" --parallel

echo
echo "Built: $ROOT_DIR/build/$BUILD_PRESET/VFXPhotoLab"
