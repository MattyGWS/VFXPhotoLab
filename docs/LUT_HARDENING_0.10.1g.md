# 0.10.1g LUT UI, persistence and hardening contract

## Inspector contract

A loaded LUT reports its embedded title/source label, 1D and 3D dimensions, operator profile, Generic processing mode, interpolation, exact per-channel input domains and the directive source that established each domain. Generic 3D LUTs expose Trilinear and Tetrahedral selection; named Tony and AgX profiles lock tetrahedral sampling because it is part of their defined pipeline.

The evaluation status distinguishes:

- exact 16-bit CPU compositing;
- table values outside finite RGBA16Float range;
- domains outside safe f32 uniform range;
- packed lookup dimensions above the conservative native limit;
- unavailable WebGPU/base parity;
- LUT-specific startup parity rejection;
- approved RGBA16Float WGSL evaluation.

The complete hierarchy may still select a local CPU fallback because of another layer or group. This does not alter the LUT's persisted contract.

## Persistence

The LUT table is embedded in `AdjustmentData`. Title, source label, domains and origins, interpolation, processing, operator profile and Strength therefore survive layer duplication, Undo/Redo state copies, public project save/reopen, adjustment presets and private Hot/Warm/Cold snapshots without requiring the original `.cube` file.

No schema changed in this stage: adjustment 10, public project 14 and private residency 15.

## Preset safety

Adjustment preset JSON is capped at 32 MiB before `readAll()` and before atomic write. A maximum supported 65³ float LUT remains comfortably below the limit. Existing preset storage names are not overwritten unless the user explicitly confirms replacement.
