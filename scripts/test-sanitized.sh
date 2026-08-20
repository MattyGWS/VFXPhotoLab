#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export VFXPHOTOLAB_CMAKE_PRESET=sanitized
"$ROOT_DIR/scripts/build-linux.sh"
export ASAN_OPTIONS="${ASAN_OPTIONS:-halt_on_error=1:abort_on_error=1:detect_leaks=0:strict_string_checks=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"
ctest --preset sanitized
