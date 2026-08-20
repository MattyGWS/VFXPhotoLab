# Local wgpu-native SDK location

VFX Photo Lab 0.3.0a pins wgpu-native **29.0.1.1** for the first native GPU checkpoint.

The normal Linux and Windows build scripts automatically download the matching release into this directory. Platform binaries and the local version marker are intentionally ignored by Git; this README remains tracked. The fetchers verify the official release SHA-256 before installation.

Expected layout:

```text
include/webgpu/webgpu.h
include/webgpu/wgpu.h
lib/libwgpu_native.so          # Linux
bin/wgpu_native.dll            # Windows, depending on release layout
lib/wgpu_native.lib            # Windows import library
```

Manual SDKs are also supported through `VFXPHOTOLAB_WGPU_ROOT` or the `WGPU_ROOT` environment variable.

Set `VFXPHOTOLAB_SKIP_WGPU_FETCH=1` before running the build script to force a CPU-only build. The application remains fully usable without WebGPU; the About dialog explains which path was selected.
