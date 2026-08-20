# Contributing

VFX Photo Lab is at an early architectural stage. Small, focused changes are easier to review than broad feature additions.

## Before submitting a change

- Build with the Debug preset.
- Run `ctest --preset debug --output-on-failure`.
- For lifetime, bounds or shutdown defects, reproduce with `./run-sanitized.sh` and run `./scripts/test-sanitized.sh` before changing allocator-sensitive code.
- Keep `PhotoDocument` as the source of truth; view widgets must not own canonical layer structure.
- Keep document processing outside UI classes where practical.
- Preserve nondestructive editing: adjustments must not overwrite the embedded source image.
- Avoid blocking the UI thread with full-frame or full-resolution processing.
- Route visible document mutations through the command/history layer.
- Add dirty-region/full-render parity tests when changing compositing.
- Preserve CPU fallback behaviour when adding GPU work.
- Do not publish stale or partially written GPU/tool results.
- Tag asynchronous document work with the current document-session identity and reject it before model or cache publication when the session changes.
- Use atomic replace-on-success file output for projects and flattened exports; never truncate a valid user file before validation and encoding complete.
- Include malformed-file checks for codecs and project parsing.
- Update architecture notes when a design boundary changes.

## GPU work

The canonical entry point is `RenderBackend`; tools should submit semantic dirty-region work rather than calling a GPU API directly. Native WebGPU lifetime belongs in `WebGpuContext` and future pipeline/cache classes under `src/gpu/`.

WGSL kernels live under `shaders/` and are embedded through Qt resources. A GPU implementation must match `ImageProcessor` in visual parity tests and retain graceful CPU fallback.

Do not commit local `wgpu-native` SDK binaries.

## Code style

- C++20
- Four-space indentation
- Braces on their own line for functions and control blocks
- `m_` prefix for private data members
- Qt types at UI and I/O boundaries
- Standard-library algorithms and utilities where they improve clarity

## Licensing

Contributions are accepted under GPL-3.0-or-later, matching the project licence.
