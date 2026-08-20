# 0.13.0g.15e — Vector Feather Hardening and Regression Coverage

This revision closes the 0.13.0g.15 non-destructive vector-layer Feather sequence. It deliberately preserves the data model, renderer equations and workflow decisions established in 15a–15d rather than adding another feature layer.

## Rendering invariants

- `0.0 px` remains an exact branch to the accepted semantic vector rasteriser. Tests render RGBA8 and RGBA64 at zero, render a non-zero Feather revision, return to exactly zero, and require byte-identical output.
- Tiny geometry remains valid input. The persisted maximum `1,000,000 px` radius is evaluated from compact authored geometry and a requested output region; it is never used as the dimensions of an empty intermediate image.
- Fill-only, stroke-only and fill+stroke vectors remain one combined coverage silhouette. Dashed strokes, square caps, bevel joins, compound paths and EvenOdd/NonZero winding receive explicit final regressions in addition to the existing holes, open paths, arrowheads and off-canvas cases.
- RGBA8/RGBA64 references use the same coverage equation. Differences are limited to expected storage quantisation; authored RGB remains outside the coverage filter.

## GPU memory guard

`VectorRasterizer::prepareGpuFeatherTile()` already had a 256 MiB working-set guard, but its estimate did not conservatively include all temporary image storage used while constructing the exact authored-colour carrier. 15e counts semantic/coverage images, possible conversion detachments, prepared coverage, nearest-X scratch, output carrier storage and small prefix/envelope overhead before allocation. Oversized direct requests therefore reject earlier and use the complete exact CPU renderer. Normal compositor tiles and the parity equation are unchanged.

## Cache and residency hardening

The shared vector raster cache remains capped at 128 MiB and 2048 entries. The final regression matrix drives many distinct Feather revisions and verifies those ceilings so continuous scrubbing cannot retain every historical raster indefinitely.

Private Hot/Warm/Cold snapshots remain format 17. A regression rewrites only the envelope version of a valid non-zero-Feather snapshot to 16 and verifies restore is rejected rather than accepting Feather state that could not have existed in that format. Public projects remain version 16 and vector data remains schema 8.

## Existing integrations retained

15e does not alter layer opacity/blending, raster masks, adjustment masks, Isolated/Pass Through groups, transforms, off-canvas placement, Merge/Expand Stroke decisions, SVG round-trip metadata, Quick Export, Production Export, ICC/OCIO/ACES processing, recovery or multi-document residency. These paths continue to consume the shared renderer audited in 15d.

The current editable vector appearance model stores solid fill/stroke colour and opacity. It does not contain a separate editable vector-gradient paint payload, so this hardening pass does not invent one or conflate the raster Gradient Tool with vector appearance.

## Compatibility

- Vector schema: **8**
- `.vfxphoto` project format: **16**
- Hot/Warm/Cold snapshot format: **17**
- No export-profile, production-plan, queue or recovery envelope change

With 15e complete, the next planned major milestone is **0.14.0 — Smart Layers and Live Filters**.
