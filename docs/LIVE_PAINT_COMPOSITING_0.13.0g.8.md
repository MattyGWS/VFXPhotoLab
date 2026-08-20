# 0.13.0g.8 — Incremental Live Paint Compositing

## Accepted baseline and scope

This revision is based only on the accepted 0.13.0g.7 source tree. It addresses the first in-between-milestones blocker: severe live-stroke slowdown when the edited raster layer sits beneath several adjustment layers. It does not begin 0.14.0 and does not change adjustment arithmetic, persistent schemas or final rendered results.

## Trace findings

A live stroke already maintained a raster working image and calculated a document dirty rectangle for each incremental update. The expensive step occurred after the dab was stamped: ordinary composite views sent that dirty rectangle through the persistent tiled canvas renderer.

For every transient pointer update, that renderer rebuilt composite identity by walking the contributing layer tree, copying relevant pixel regions and hashing their bytes. It then expanded even a small dab to complete 256 × 256 cache tiles, composited the adjustment hierarchy and inserted a cache generation that the following dab immediately made obsolete. The cost therefore grew with the number of contributing raster/mask surfaces and adjustment layers even though most pixels and most of the document were unchanged.

The actual adjustment evaluation was region bounded already. Spatial adjustments still request their exact dependency halo; ordinary tonal/colour adjustments operate only on the dirty region. The primary avoidable work was persistent cache identity and tile-granularity churn during a transient stroke.

## Implementation

Brush-family, eraser, Dodge/Burn/Sponge, Blur/Sharpen, Smudge, Clone Stamp, Healing and Patch live composite updates now request the existing interactive hierarchy compositor:

- Approved 8-bit hierarchies use one native WebGPU submission and one readback for the requested region.
- A new explicit transient fallback mode renders the exact bounded region directly with `ImageProcessor::renderRegion` when the GPU is unavailable, the document is 16-bit or a colour/adjustment parity gate requires the CPU reference.
- Transient fallback does not query, populate or invalidate the persistent 256 × 256 tile cache.
- Non-paint interactive callers keep the previous bounded tiled fallback under GPU resource pressure, so large adjustment-slider and gradient previews do not lose their tiled GPU escape path.

The existing post-stroke commit and scheduled persistent preview render remain unchanged. Stroke release therefore still publishes the normal final-quality image and persistent cache generation.

## Preserved contracts

The implementation continues to use the existing layer tree and region compositor, preserving:

- exact 8-bit and 16-bit processing paths;
- Alpha-safe RGB beneath zero Alpha;
- raster and adjustment masks;
- selections and editable RGB/Alpha channels;
- nested isolated and Pass Through groups;
- spatial-filter dependency halos;
- ICC, OpenColorIO, ACES, display management and soft proofing state;
- Hot/Warm/Cold document identity and render-session cancellation;
- one grouped Undo entry per completed stroke;
- project, preset, export, queue and recovery compatibility.

## Regression coverage

The interactive CPU fallback test now records tile-cache statistics, requests the transient fallback, compares its image with the ordinary tiled renderer and verifies that resident tiles, dirty tiles, hits and misses are unchanged by the transient request.

## Follow-up traces retained

Crop rotation uses a preview-only canvas rotation and a separate apply-time document transform construction. Non-zero-angle crop output has no focused regression coverage, so that path remains isolated for the next revision rather than being changed speculatively.

The Tool Options toolbar remains a fixed 44 px row containing 30 px scrub widgets, while Qt styles the internal spin-box and line-edit subcontrols. The remaining Fedora KDE clipping is therefore a child/subcontrol geometry and device-pixel rounding issue, not justification for increasing the toolbar height again.
