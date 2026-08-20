# 0.13.0g.15c — Native Tiled GPU Feather Rendering

## Scope

This revision adds native tiled GPU execution for the non-destructive vector-layer `featherRadius` introduced in 0.13.0g.15a and defined by the exact CPU reference in 0.13.0g.15b. It does not begin Smart Layers or Live Filters, and it changes no persisted schema.

## Semantic preparation

Vector geometry remains editable and authoritative. For each contributing tile, the existing QPainter vector resolver produces a compact ordinary appearance surface and one combined fill/stroke coverage surface. The coverage includes compound fill rules, holes, open-path stroke geometry, inside/centre/outside strokes, dashes, caps, joins and arrowheads before Feather is applied. A deterministic CPU preparation step also derives the nearest-authored-colour carrier and separates local style Alpha from antialias coverage. No fill or stroke RGB is uploaded as filter input.

## Native kernel

`shaders/vector_feather.wgsl` contains two compute entry points. The horizontal pass convolves coverage Alpha with the exact uploaded fractional three-box X kernel into a float storage buffer. The vertical pass applies the Y kernel, multiplies by carrier style Alpha, preserves carrier RGB and writes straight `rgba8unorm`. Signed source and output origins keep global tile addressing correct for negative/off-canvas geometry.

The kernel generator uses the same three-pass integer support distribution as the CPU reference and linearly combines adjacent exact integer kernels for typed fractional radii. This is not an ordinary RGBA Gaussian blur and it does not average fill/stroke colours.

## Parity and fallback

Startup validation compares the native kernel with a CPU two-pass reference containing fractional anisotropic supports, a hole, partial coverage and varying semi-transparent carrier colours. Vector Feather is GPU-approved only when the maximum channel difference is at most one. Approval is serialized through the existing isolated helper validation result.

The complete exact CPU tile is used when:

- the GPU/device or feature parity gate is unavailable;
- the document is 16-bit;
- preview support exceeds the approved 256-pixel native bound;
- source/output resource guards are exceeded;
- shader, pipeline, allocation, bind, submission or readback fails.

Interaction never substitutes a cheaper kernel or temporarily disables Feather.

## Tiling and cache locality

Each request expands source coverage by the scaled Feather support and intersects that support with compact semantic vector bounds. Full and stitched tiles use the same global coordinates. Feather-expanded content bounds allow geometry wholly outside the document to contribute back into a canvas tile.

Composite revision hashing records a contribution marker per vector and only includes vector fingerprint, world transform and layer revision for tiles touched by the expanded silhouette. A Feather, geometry or transform change therefore keeps unrelated tiles resident while correctly invalidating tiles entered, left or changed by the silhouette. Ordinary raster masks remain separate hierarchy inputs.

## Compatibility boundary

Vector schema 8, public project version 16 and Hot/Warm/Cold snapshot version 17 are unchanged. The CPU renderer remains the reference for 8-bit parity and the exact path for 16-bit output and all native fallbacks. Broader command/export/SVG integration is reserved for 0.13.0g.15d.
