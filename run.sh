#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY="$ROOT_DIR/build/release/VFXPhotoLab"

# Always ask CMake/Ninja to update the application before launching it.
# The build is incremental, so unchanged files are not recompiled.
"$ROOT_DIR/scripts/build-linux.sh"

if [[ ! -x "$BINARY" ]]; then
    echo "Build completed, but the VFX Photo Lab executable was not found at:" >&2
    echo "  $BINARY" >&2
    exit 1
fi

exec "$BINARY" "$@"
