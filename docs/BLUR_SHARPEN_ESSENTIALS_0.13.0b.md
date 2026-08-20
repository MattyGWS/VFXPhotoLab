# 0.13.0b — Blur and Sharpen Essentials

## Scope

This stage adds the first public consumers of the 0.13.0a spatial-filter foundation:

- Gaussian Blur
- Box Blur
- Unsharp Mask
- High Pass

They are ordinary non-destructive adjustment layers. They participate in existing masks, group modes, opacity, blend modes, presets, Undo, project persistence, Hot/Warm/Cold snapshots, export snapshots and queue recovery. Smart Layers and general Live Filter stacks remain 0.14.0 work.

## Processing model

All four operations declare a document-space support radius through `adjustmentSpatialRadius()`. `maximumSpatialAdjustmentRadius()` composes those supports across the complete hierarchy, so stacked filters receive the cumulative source dependency required to render a tile without seams. The normal preview path scales both the halo and the kernel radius by the document-to-preview ratio.

The public radius is bounded to 0–500 px. Halo materialisation and deterministic CPU working memory retain the stricter 0.13.0a allocation guards. Long loops observe the render cancellation flag and publish no partial image.

### Gaussian Blur

Gaussian Blur uses three deterministic separable box passes. The requested integer support is distributed across those passes and their radii sum exactly to the declared tile halo. This gives a smooth Gaussian approximation with work proportional to pixel count rather than radius. Remainders are assigned in a stable order, making cache and test results deterministic.

### Box Blur

Box Blur uses one exact separable sliding-window pass. It is exposed as a useful graphic blur and as a direct diagnostic of the shared spatial path.

### Unsharp Mask

Unsharp Mask subtracts the Gaussian approximation from straight source RGB, applies Amount and adds the detail back. Threshold is stored in 8-bit code-value units from 0–255 and converted to a normalised threshold for both 8-bit and 16-bit documents. Alpha is copied exactly from the source.

### High Pass

High Pass returns neutral grey plus source minus Gaussian blur. Monochrome mode computes the detail from Rec.709 luminance and writes it equally to RGB. Alpha is copied exactly. Overlay remains an ordinary layer blend mode rather than a hidden filter option.

## Alpha and hidden RGB

Gaussian and Box Blur expose two Alpha policies:

- **Blur transparency / Alpha enabled:** filter coverage-aware premultiplied RGB and Alpha, with a separately filtered straight-RGB fallback where resulting Alpha is zero.
- **Disabled:** filter straight RGB while preserving source Alpha exactly.

Unsharp Mask and High Pass always preserve source Alpha. The independent RGB-reference render used by export, channel editing and hidden-colour restoration remains active, so zero-Alpha pixels do not become forced black merely because an adjustment was evaluated.

## Colour and precision contract

The four filters operate in the document's encoded working-component domain. They do not reinterpret the ICC profile, OCIO configuration, ACES role, display transform, proof transform or output profile. Managed colour transformations around other adjustments remain unchanged.

RGBA8 and RGBA64 have separate deterministic writeback paths. Gaussian and Box Blur use the shared float-plane reference and quantise only at pass output. Unsharp Mask and High Pass calculate from normalised components and write back at the source precision. Alpha is retained at that precision when preservation is selected.

## 0.13.0b.2 performance hardening

The CPU reference now receives a bounded row scheduler from `ImageProcessor`. Each scalar component is extracted and written in parallel, and the exact sliding-window kernel distributes both horizontal rows and vertical columns across the processing pool. Unsharp Mask and High Pass also parallelise their final detail-composition rows. Buffers are detached once before dispatch, and each worker owns disjoint output rows or columns; cancellation and deterministic summation order within each pixel remain unchanged.

A continuous Gaussian Blur or Box Blur slider gesture over a large visible region may use an existing document preview mip chosen to keep the interactive CPU footprint near 512×512 pixels. Radius and halo are scaled by the same document-to-preview contract. Full-resolution export, Production Export, queue snapshots and recovery never use the interaction mip.

## 0.13.0b.3 flicker-free interaction handoff

The interaction mip is retained across same-size render generations until a complete replacement is validated. New pointer events no longer clear the currently displayed mip and reveal the older sharp backing while the next CPU frame runs. On mouse release, the final interaction mip remains visible until the atomic level-0 visible region commits, then the transient mip is discarded. This changes presentation lifetime only; filter arithmetic, final pixels, Undo, persistence and export snapshots are unchanged.

## 0.13.0b.4 detail-accurate Sharpen interaction

Unsharp Mask and High Pass do not use the blur interaction mip. Both evaluate the visible viewport from level-0 source data while their controls are dragged, because a downsampled source removes fine texture, edge contrast and threshold transitions and therefore cannot represent a sharpening result. Full-detail frames retain the complete-frame atomic replacement rule from 0.13.0b.3.

The most recent completed level-0 sharpen frame is also eligible to become the settled result directly. Mouse release does not automatically request a second identical generation: a completed current frame settles immediately, while an already running or coalesced current frame finishes once and then resumes normal thumbnail and residency bookkeeping. Blur still performs its required level-0 release pass when its live frame used a mip.

## GPU approval and fallback

Existing native WebGPU compositing remains feature-gated. The new adjustment enum values have stable GPU identifiers, but no approval bit is issued in 0.13.0b because dedicated WGSL kernels have not yet passed parity validation. A hierarchy containing one of these filters therefore selects the deterministic CPU reference. Hierarchies containing only previously approved adjustments remain eligible for native GPU compositing.

This is an explicit fallback, not an attempted shader execution. It prevents a driver or partial shader implementation from silently changing pixels while the future reusable live-filter GPU path is developed.

## Persistence compatibility

The public `.vfxphoto` project format remains 15, private residency snapshot schema remains 16, colour-state schema remains 4 and vector schema remains 7.

Adjustment schema advances from 10 to 11 to serialize the four appended types and their parameters. Existing enum ordering is unchanged: Shadows/Highlights remains identifier 15 and the new filters occupy 16–19. Schemas 1–10 retain their established parsing and migration rules. Export profiles, production plans, queue descriptions and recovery envelopes do not gain new fields; their existing immutable layer snapshot simply carries adjustment schema 11 when one of the new filters is present.

## Deterministic coverage

`VFXPhotoLabSpatialFilterTests` now covers:

- append-only enum identity and adjustment JSON round-trips;
- cumulative support accounting across a filter stack;
- deterministic Gaussian output;
- Gaussian full-frame versus halo-cropped tile equivalence;
- Box Blur edge and Alpha modes;
- hidden-RGB and source-Alpha preservation;
- cancellation and allocation guards;
- full-frame versus 256×256 tiled rendering for Gaussian Blur, Box Blur, Unsharp Mask and High Pass.
