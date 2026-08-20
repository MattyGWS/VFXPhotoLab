# 0.14.0e — Tiled Smart Rendering and Cache Architecture

## Scope

0.14.0e turns the source-backed Smart transform work from 0.14.0d into a reusable tiled rendering contract. It does **not** add Live Filters, Layer Effects or linked external sources. Those remain 0.14.0f onward.

The authoritative model remains:

`Embedded Smart Source contents → source revision snapshot → instance transform → instance mask → opacity/blend`

The new cache architecture sits between the source revision snapshot and parent composition. No transformed cache becomes authoritative and no cache is serialized into `.vfxphoto` or Hot/Warm/Cold snapshots.

## Source-tile requests

`SmartLayerTileCache` divides Smart source presentations into 256×256 source tiles. Source tiles are converted once to straight `RGBA64`, preserving hidden RGB independently of Alpha and giving Bicubic/Lanczos the same exact unassociated 16-bit input used by the 0.14.0d reference sampler.

A transformed output request inverse-maps only its requested parent-space region into Smart source space. The requested footprint is expanded for interpolation support:

- Nearest: one-pixel safety support.
- Bilinear: two-pixel compatibility support.
- Bicubic: three-pixel support.
- Lanczos 3: four-pixel support.

Only source tiles overlapping that footprint are assembled into the temporary source patch.

## Intermediate transformed cache

Transformed Smart output regions are cached before layer mask, opacity and blend. This is intentionally separate from `TiledCanvasEngine`'s final composite cache.

That distinction solves the important lower-layer editing case:

1. A Smart Layer is expensive to scale/rotate/filter in source space.
2. A raster layer underneath changes.
3. The final composite tile is correctly dirty.
4. The Smart transform itself has not changed.
5. The new composite can therefore reuse the cached Smart intermediate instead of resampling the Smart source.

Smart instance masks have their own transformed intermediate keys so the same rule applies to masked Smart Layers.
The native hierarchy path requests the same exact Smart-mask tile used by CPU composition rather than independently applying a generic smooth transform, so mask/content registration and cache identity remain consistent for Nearest, Bilinear, Bicubic and Lanczos 3.

The default Smart RAM budget is 192 MiB. Source and transformed images share one LRU budget. Eviction is deterministic and never changes correctness because all entries are derived.

## Content-addressed dirty propagation

A Smart Source revision is an authoritative dependency revision, but it is deliberately **not** sufficient by itself to define a parent output tile.

For each output tile, 0.14.0e computes the exact inverse-mapped source overlap that the tile can sample and fingerprints only those pixels. The fingerprint includes hidden RGB because source tiles are evaluated as straight RGBA64.

Consequences:

- Editing Smart source pixels on the far side of a large source can advance the source revision while leaving an unrelated parent output tile reusable.
- Editing pixels inside the sampled footprint changes the fingerprint and invalidates that tile.
- Moving/rotating a Smart Layer wholly outside another parent tile no longer invalidates the unaffected tile.
- Undo/branch-edit safety is retained because source-tile/fingerprint lookup is still revision/image-identity scoped while transformed results are reused across revisions only when their exact sampled content fingerprint matches.

Region-fingerprint metadata is independently capped at 65,536 entries and is LRU aged.

## Native GPU residency

Prepared Smart parent-space tiles receive a deterministic residency key derived from the same source-content/transform semantics as the CPU intermediate cache. The existing WebGPU resident-texture LRU can therefore retain them between hierarchy-composite submissions.

When a lower layer dirties the final composite but the Smart intermediate is unchanged, the hierarchy compositor borrows the resident Smart texture directly rather than uploading the same `QImage` again.

Resident textures remain owned by `WebGpuContext` and bounded by its existing VRAM budget. A residency miss falls back to the normal exact upload/composite path; there is no fake preview or silently disabled processing.

Smart resident keys are registered against the owning `DocumentSession`. When Hot/Warm/Cold moves that session to Cold, `RenderBackend::releaseSession()` releases those native Smart intermediates along with the session's normal renderer residency. CPU Smart source/transformed/mask entries for the session are purged at the same Cold transition.

## Source revision snapshot boundary

The embedded `LayerNode` tree remains authoritative. `SmartSourceDescriptor::presentationImage` remains a derived stable snapshot of one committed source revision, generated at conversion/Edit Contents save boundaries. 0.14.0e does not repeatedly materialize or transform that full image during parent interaction: parent rendering addresses it through tile-local source footprints and intermediate caches.

This preserves the existing exact nested/group/adjustment rendering semantics while establishing the cache/request contract that 0.14.0f Live Filters can consume. A future implementation may make embedded source revision production itself more incremental without changing the parent-facing contract introduced here.

## Persistence and residency

The caches are runtime derivations and are never serialized. Therefore this revision intentionally keeps:

- `.vfxphoto` project format: **20**
- embedded Smart document schema: **3**
- private Hot/Warm/Cold snapshot format: **21**

On project load or cold restore, authoritative Smart state is restored normally and RAM/VRAM intermediates are repopulated on demand.

## Regression contract

0.14.0e specifically guards:

- unchanged transformed Smart reuse after painting below it;
- unchanged transformed mask reuse;
- bounded RAM use and LRU eviction;
- far source edit reuse across a source revision bump;
- near source edit invalidation;
- Smart transform changes outside a tile leaving that tile cached;
- exact source dirty propagation in the final composite cache;
- hidden-RGB-safe 8/16-bit source requests;
- CPU fallback correctness when native residency is unavailable.
