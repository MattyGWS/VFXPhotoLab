# 0.12.0g — Presets and Production Export integration hardening

This stage closes the 0.12.0 milestone by hardening the boundaries between the unified preset library, export profiles, resolved multi-output plans, the modeless queue and private recovery descriptions. It does not add folder discovery, batch automation or another pixel pipeline.

## Resolved-output trust boundary

`resolveProductionExportPlan()` remains the dialog/recovery preflight that calculates paths and collision observations. Before a job becomes queue-owned, `validateResolvedProductionExportOutputs()` now proves that the caller-supplied resolved vector is a faithful one-to-one snapshot of every enabled plan row.

Validation covers stable output/profile identifiers, captured profile name/settings, resize dimensions and resampler, safe surface limits, portable filename resolution, output-directory containment, canonical auto-rename siblings, unique destinations, colour/Alpha/dither/quality parity and writer availability. Tampered, stale or partial vectors are rejected before any recovery file or queue record is accepted.

## Side-effect-free queue acceptance

Queue acceptance now follows this order:

1. Validate source format, dimensions, colour state and processing contract.
2. Validate the resolved production payload against the plan.
3. Allocate a queue identifier that does not collide with memory or disk.
4. Write the recoverable record atomically.
5. Only then trim bounded terminal history and publish the new job.

A failed enqueue therefore cannot evict history or leave a partially accepted job.

## Collision and progress correctness

`skipExisting` records what dialog preflight observed. The worker always rechecks the destination when the queued output actually runs. A file that still exists is skipped; a file deleted while the job waited is exported normally. Every configured output advances progress exactly once whether it completes, skips or fails, in addition to the shared render unit.

## Preserve-on-quit completion race

A cancellation request can arrive after the worker has atomically written its final output. Preserve-on-quit now inspects the completed worker result. Only an exact, non-cancelled result containing one completed-or-skipped entry for every expected output is finalised. Its recovery record is removed. Any missing, failed, cancelled or render-failed result remains recoverable.

## Recovery and atomic-write hardening

Layer-tree validation uses an iterative bounded traversal, enforcing maximum node count, maximum depth, unique IDs and non-null IDs without recursive stack growth. Queue snapshots must use straight RGBA8/RGBA16 and a colour-processing contract matching the captured colour state.

Recovery persistence checks available storage before writing. `QSaveFile` direct-write fallback is explicitly disabled for recovery descriptions, normal image/TGA exports and unified preset state, so inability to complete an atomic replacement fails visibly instead of silently degrading to an in-place write.

## Compatibility

No public or private document schema changes:

- `.vfxphoto` project format: 15
- Hot/Warm/Cold snapshot schema: 16
- colour-state schema: 4
- adjustment schema: 10
- vector schema: 7

Existing quick export, ICC/OCIO/ACES conversion, GPU/CPU fallbacks, hidden RGB, editable channels, document residency, Smudge and 0.11.0i.3 shutdown ordering remain the underlying contracts.
