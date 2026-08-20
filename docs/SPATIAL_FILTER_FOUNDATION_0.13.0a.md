# 0.13.0a — Spatial Filter Foundation

## Purpose

This stage establishes one reusable spatial-processing boundary before public blur, sharpen and optical filters are added. It deliberately avoids creating placeholder UI or persisted filter types that would later need migration.

## Traced baseline

The implementation was integrated with the accepted 0.12.0g source tree after tracing adjustment serialization, CPU/WGSL evaluation, the per-feature GPU parity gate, native 256×256 tiled compositing, tile revisions, Shadows/Highlights halos, masks/groups/Pass Through, selection-local editing, straight-RGBA channel storage, RGBA8/RGBA64 paths, colour-management domains, presets, Undo, project/residency persistence, multi-document session identity, export/queue snapshots and shutdown ordering.

The baseline already had a correct but duplicated scalar halo calculation for Shadows/Highlights in full-resolution and tiled renderers. 0.13.0a centralises that behaviour and retains its conservative two-pixel safety margin.

## Core contracts

### SpatialFilterContract

- document-space X/Y radius;
- Clamp, Mirror, Wrap or Transparent edge mode;
- Straight RGBA, Preserve Source Alpha or Coverage-Aware RGBA processing;
- Interactive or Final quality identity;
- bounded safety padding and maximum radius;
- stable non-zero fingerprint.

The contract is normalised before use and is intentionally not serialized in this stage.

### SpatialFilterTilePlan

A plan contains the clipped output region, potentially out-of-bounds sampling rectangle, in-bounds dependency regions, conservative dependency bounds, scaled radius, crop offset, preview scale, failure reason and cache fingerprint. Clamp/Transparent dependencies clip normally; Wrap can return opposite-edge regions; Mirror conservatively returns the complete extent when reflection crosses an edge.

`affectedOutputRegions()` supplies the inverse mapping needed when later Live Filter caches invalidate outputs after a source dirty region changes.

### Host/WGSL layout

`SpatialFilterGpuContract` is 64 bytes, 16-byte aligned and has compile-time field-offset assertions. `shaders/spatial_filter_fixture.wgsl` mirrors that layout and validates edge-mapped document coordinates. The fixture is not a public image filter.

## Alpha and precision

Halo extraction converts to straight `RGBA8888` or `RGBA64`, preserves colour-space and resolution metadata, and copies all four components exactly. Hidden RGB remains meaningful when Alpha is zero. Transparent edges synthesize zero RGBA only outside the document.

The deterministic internal Box Blur fixture validates:

- all four edge modes;
- straight-RGBA, source-Alpha-preserving and coverage-aware contracts;
- tile/full-frame byte equality across document and 256-pixel tile boundaries;
- hidden RGB;
- RGBA8/RGBA64 consistency;
- cancellation and memory rejection.

It is test infrastructure, not a user-visible Box Blur implementation.

## Existing production integration

Shadows/Highlights keeps the same 13 weights, radius-scaled offsets, RGBA8 horizontal quantisation, straight-Alpha behaviour and exact CPU/WGSL parity assumptions. Its large-image CPU passes remain parallel. The existing startup helper remains the authority that approves or rejects the WGSL spatial adjustment and selects the exact CPU fallback.

Both `ImageProcessor` and `TiledCanvasEngine` now use the shared plan for dependency regions. Tiled composite revisions additionally hash the plan fingerprint, creating the cache/invalidation seam needed by later Live Filters.

## Safety and compatibility

- Maximum normalised radius: 4096 pixels per axis.
- Maximum explicit halo: 512 MiB at worst-case RGBA64.
- Maximum deterministic reference float working set: 768 MiB.
- Cooperative cancellation is checked before publication and throughout long loops.
- Invalid large plans conservatively render from the full existing source region.
- No project, adjustment, colour, vector, residency, preset, export, queue or recovery schema changed.
- No Smart Layer or Live Filter stack is implemented yet.
