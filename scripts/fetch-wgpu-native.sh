#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${VFXPHOTOLAB_WGPU_VERSION:-29.0.1.1}"
TARGET_DIR="${WGPU_ROOT:-$ROOT_DIR/third_party/wgpu-native}"
RELEASE_BASE="${VFXPHOTOLAB_WGPU_RELEASE_BASE:-https://github.com/gfx-rs/wgpu-native/releases/download/v${VERSION}}"

sdk_ready() {
    local root="$1"
    [[ -f "$root/include/webgpu/webgpu.h" || -f "$root/include/webgpu-headers/webgpu.h" ]] || return 1
    [[ -f "$root/lib/libwgpu_native.so" || -f "$root/lib64/libwgpu_native.so" || \
       -f "$root/lib/libwgpu_native.a" || -f "$root/lib64/libwgpu_native.a" || \
       -f "$root/lib/libwgpu.so" || -f "$root/lib64/libwgpu.so" ]] || return 1
}

if sdk_ready "$TARGET_DIR"; then
    echo "wgpu-native ${VERSION} is already present."
    exit 0
fi

case "$(uname -m)" in
    x86_64|amd64) archive_arch="x86_64" ;;
    aarch64|arm64) archive_arch="aarch64" ;;
    i386|i486|i586|i686) archive_arch="i686" ;;
    armv7|armv7l|armhf) archive_arch="armv7" ;;
    *)
        echo "Unsupported Linux architecture for the pinned wgpu-native SDK: $(uname -m)" >&2
        exit 1
        ;;
esac

archive_name="${VFXPHOTOLAB_WGPU_ASSET:-wgpu-linux-${archive_arch}-release.zip}"
archive_url="${VFXPHOTOLAB_WGPU_URL:-${RELEASE_BASE}/${archive_name}}"

for tool in unzip find mktemp; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "Missing required tool: $tool" >&2
        exit 1
    }
done
if command -v curl >/dev/null 2>&1; then
    downloader=(curl --fail --location --retry 3 --connect-timeout 20 --output)
elif command -v wget >/dev/null 2>&1; then
    downloader=(wget --quiet --output-document)
else
    echo "curl or wget is required to download wgpu-native." >&2
    exit 1
fi

temp_dir="$(mktemp -d "${TMPDIR:-/tmp}/vfxphotolab-wgpu.XXXXXX")"
stage_dir="$(dirname "$TARGET_DIR")/.wgpu-native-stage-$$"
cleanup() {
    rm -rf "$temp_dir" "$stage_dir"
}
trap cleanup EXIT

archive_path="$temp_dir/$archive_name"
extract_dir="$temp_dir/extract"
mkdir -p "$extract_dir" "$(dirname "$TARGET_DIR")"

echo "Downloading wgpu-native ${VERSION} (${archive_name})..."
"${downloader[@]}" "$archive_path" "$archive_url"
unzip -q "$archive_path" -d "$extract_dir"

include_file="$(find "$extract_dir" -type f \( \
    -path '*/include/webgpu/webgpu.h' -o \
    -path '*/include/webgpu-headers/webgpu.h' \
\) -print -quit)"
if [[ -z "$include_file" ]]; then
    echo "The downloaded archive does not contain the expected WebGPU headers." >&2
    exit 1
fi

if [[ "$include_file" == */include/webgpu/webgpu.h ]]; then
    release_root="${include_file%/include/webgpu/webgpu.h}"
else
    release_root="${include_file%/include/webgpu-headers/webgpu.h}"
fi

mkdir -p "$stage_dir"
cp -a "$release_root"/. "$stage_dir"/

# Some archives place headers and libraries in sibling roots. Fill those gaps
# without assuming a single archive layout.
if [[ ! -d "$stage_dir/include" ]]; then
    include_dir="$(dirname "$(dirname "$include_file")")"
    mkdir -p "$stage_dir/include"
    cp -a "$include_dir"/. "$stage_dir/include"/
fi
if ! sdk_ready "$stage_dir"; then
    library_file="$(find "$extract_dir" -type f \( \
        -name 'libwgpu_native.so' -o -name 'libwgpu_native.a' -o \
        -name 'libwgpu.so' -o -name 'libwgpu.a' \
    \) -print -quit)"
    if [[ -n "$library_file" ]]; then
        mkdir -p "$stage_dir/lib"
        cp -a "$(dirname "$library_file")"/. "$stage_dir/lib"/
    fi
fi

if ! sdk_ready "$stage_dir"; then
    echo "The downloaded archive could not be normalised into include/ and lib/." >&2
    exit 1
fi

printf '%s\n' "$VERSION" > "$stage_dir/.vfxphotolab-version"
rm -rf "$TARGET_DIR"
mv "$stage_dir" "$TARGET_DIR"
trap - EXIT
rm -rf "$temp_dir"

echo "Installed wgpu-native ${VERSION} to $TARGET_DIR"
