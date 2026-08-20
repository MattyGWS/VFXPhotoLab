# 0.11.0g — GPU/WGSL Colour Transform Integration

## Boundary

0.11.0g accelerates two colour-transform boundaries while preserving the CPU implementation as the correctness reference:

1. the presentation-only pipeline introduced in 0.11.0f; and
2. the managed adjustment-domain transitions introduced in 0.11.0d.

It does not introduce an alternative document colour model and does not apply monitor, proof or OCIO Display/View transforms to raster editing, history, Merge Layers, Copy Merged or export.

```text
Presentation:
working-space presentation image
→ authoritative CPU ICC / OCIO / proof chain baked to lattice
→ parity-approved WGSL trilinear evaluation
→ derived QImage used by QPainter

Managed adjustment compositor:
working-space composite
→ parity-approved working→adjustment-domain lattice
→ existing WGSL adjustment operator
→ parity-approved adjustment-domain→working lattice
→ normal working-space blend/composite
```

The original CPU `DisplayColourTransform::apply` and `ImageProcessor` managed adjustment path remain unconditional fallbacks.

## Reference-baked transforms

ICC engines and OpenColorIO processors are not independently reimplemented in WGSL. The validated CPU transforms are evaluated over deterministic 65×65×65 RGB lattices and stored as flattened RGBA16Float textures.

Presentation lattices cover:

- working space to automatic or manual monitor ICC;
- working space to a selected document display ICC;
- optional proof ICC simulation followed by display conversion;
- OCIO `DisplayViewTransform`, including saved display/view/look;
- mixed ICC↔OCIO bridge stages already supported by the CPU pipeline.

A second presentation lattice is baked only when gamut warning is active. It represents the working→proof→working round trip used by the existing deterministic warning rule.

Managed adjustment lattices are paired:

- working space → same-primaries linear working space for scene-referred Exposure;
- working space → encoded sRGB/Rec.709 for operators whose established luminance, HSL or Oklab contract is defined there;
- the matching domain → working-space return transform before blend and compositing.

Encoded-working, raw-component and specialised LUT-contract adjustments continue to use their existing WGSL paths without an extra domain transform.

## WGSL evaluation

`shaders/display_colour_transform.wgsl` performs manual trilinear interpolation for `rgba8unorm` and `rgba16float` presentation surfaces. Input and output Alpha are not transformed. Because RGBA16Float cannot represent every 16-bit Alpha code exactly, readback restores Alpha from the source RGBA64 surface.

`shaders/adjustment_tile.wgsl` now accepts paired domain textures and a managed-domain identifier. The shader quantises at the same 8-bit boundaries as the CPU reference, evaluates the existing adjustment in its declared domain, returns to the document working space, and blends there. Straight Alpha and hidden RGB remain independent from the colour transform.

## Runtime approval and fallback

The native WebGPU diagnostic validates display transforms independently from managed adjustment-domain transforms.

Presentation validation:

1. builds a real Display P3→sRGB ICC transform;
2. compares CPU and WGSL output on deterministic straight and premultiplied RGBA8 data;
3. compares CPU and WGSL output on deterministic RGBA64 data;
4. exercises the second-LUT gamut-warning branch.

Managed adjustment validation:

1. builds Display P3 working→linear-working and working→encoded-sRGB lattice pairs;
2. compares a managed linear-working Exposure against `ImageProcessor`;
3. compares a managed encoded-sRGB Saturation against `ImageProcessor`;
4. preserves varying Alpha while checking the complete transform→operator→inverse-transform path.

Every newly baked ICC/OCIO/proof or managed adjustment lattice also runs deterministic CPU-side probes before entering a cache. A transform exceeding the four-code lattice approximation limit is rejected. A runtime managed adjustment end-to-end mismatch rejects the managed-domain GPU feature while leaving ordinary approved adjustments and other GPU features enabled.

## Cache and invalidation

Presentation and managed adjustment CPU lattices each use bounded 48 MiB process-lifetime LRU caches. Uploaded presentation and managed-domain textures use separate bounded 48 MiB GPU LRU caches.

Presentation keys include the document working space, proof settings, monitor identity/profile, OCIO config fingerprint and display/view/look selection. Managed adjustment keys include the complete working-space fingerprint, destination domain and lattice specification. Device reset or backend shutdown releases all GPU transform caches.

## QPainter and future integration

The current canvas remains QPainter/QImage based, so presentation compute is followed by readback into a derived display image. Managed adjustment transforms execute inside the existing tiled compositor and avoid the previous CPU-only wide-gamut adjustment-stack handoff when parity is approved.

This stage does not begin 0.11.0h colour-managed export, output bit-depth conversion or blue-noise dithering.
