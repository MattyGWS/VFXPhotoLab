# 0.13.0g.15b — Exact CPU Vector Feather Rendering

## Scope

This revision implements the authoritative CPU renderer for the schema-8 per-vector-layer `featherRadius` introduced in 0.13.0g.15a. It does not add Smart Layers, live filters or a native WGSL Feather kernel, and it does not change any persisted schema or envelope version.

## Rendering contract

At exactly `0.0 px`, the accepted vector renderer is called directly. Fill, stroke, antialiasing, compound paths, holes, dashes, caps, joins, arrowheads, colour conversion and requested QImage format therefore remain on the pre-Feather path.

For a positive value, the renderer produces two compact semantic surfaces from the same resolved vector geometry:

1. the ordinary authored appearance; and
2. an opaque union silhouette of every visible fill and stroke.

The silhouette reuses the established QPainterPath fill rule, additional contours, open-path handling, inside-stroke clipping and expanded stroke outlines. Fill and stroke geometry therefore contribute before Feather is evaluated.

Only silhouette Alpha is filtered. The deterministic separable kernel is the exact three-box support distribution used by `SpatialFilterFoundation::gaussianBlurReference`. Integer radii are evaluated in closed form from prefix moments. Fractional radii linearly interpolate the two adjacent exact integer kernels, so `12.5 px` is neither rounded nor written back as another value.

The implementation does not allocate a blank rectangle spanning the complete Feather radius. It intersects compact vector bounds with the support needed by the requested output and evaluates convolution values directly in global preview coordinates. A small object tens of thousands of pixels outside the document can therefore contribute correctly without a tens-of-thousands-pixel empty halo allocation. Unusually large direct full-region requests are recursively split into exact independent subregions when the working-set estimate exceeds 512 MiB; the split path copies rows directly, so it does not introduce a compositing or rounding seam.

## Colour and Alpha

RGB never enters the blur. A nearest-covered-pixel transform maps each feathered output pixel to one pixel of the unchanged authored appearance. The output copies that straight RGB exactly. Geometric antialias coverage is separated from local authored/style Alpha; the latter is multiplied by the feathered union silhouette. This preserves semi-transparent colours and avoids red/blue or fill/stroke colour bleeding across internal boundaries.

The accepted vector schema currently stores solid fill and stroke colours rather than editable vector-gradient definitions. This revision does not invent or silently flatten a new gradient schema; the Feather compositor consumes the ordinary authored semantic surface, so future semantic paint types can remain outside the coverage filter.

RGBA8 and RGBA64 are reconstructed independently before conversion to the compositor's requested format. The existing opaque-pixel channel path remains available for Alpha-safe RGB/channel presentation.

## Bounds, tiles and fallback

Semantic content bounds expand by the document-pixel Feather radius. Each tile includes all source coverage within its scaled support and uses the same global coordinates as a full render, so stitched CPU tiles are deterministic against the full-region reference. Cancellation is checked during source rasterisation, horizontal coverage evaluation, colour propagation and output construction.

This CPU implementation is the honest fallback and parity target for 0.13.0g.15c. No fake interaction preview or GPU claim is introduced here.
