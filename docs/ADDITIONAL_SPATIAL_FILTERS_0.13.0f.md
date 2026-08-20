# 0.13.0f — Additional Spatial Filters

## Scope

This stage appends Surface Blur, Motion Blur and Radial Blur as ordinary non-destructive adjustment layers. It reuses the 0.13.0a spatial-filter foundation and the 0.13.0b interaction/cancellation hardening. It does not introduce Smart Layers or a public live-filter stack.

## Filters

### Surface Blur

Surface Blur uses the deterministic separable Gaussian reference as its local low-pass source, then suppresses that blur where the maximum straight-RGB difference from the source exceeds the selected Threshold. Radius is measured in document pixels. Threshold is stored in stable 0–255 code-value units so equivalent 8-bit and 16-bit documents behave consistently. Source Alpha is preserved exactly while hidden RGB under zero Alpha remains editable and filtered.

### Motion Blur

Motion Blur samples a centred line whose complete length is Distance and whose direction is Angle. Quality selects a bounded maximum of 4–64 bilinear samples; shorter trails automatically use fewer samples. The dependency halo is projected independently onto X and Y, avoiding an unnecessarily square source request. Transparency/Alpha may be preserved or blurred with coverage-aware colour reconstruction.

### Radial Blur

Radial Blur provides Spin and Zoom modes around an adjustable document-relative centre. Amount is the maximum complete local sampling span at the furthest document edge, expressed in pixels; displacement falls to zero at the centre. This bounded definition gives the tile planner an exact finite halo and avoids the unbounded footprints associated with percentage-only radial blur formulations. Quality and Alpha behaviour match Motion Blur.

## Rendering and compatibility

All three filters use exact multicore CPU references until separate WGSL implementations pass the existing parity gate. They participate in masks, opacity, blend modes, Isolated and Pass Through groups, Undo, presets, project persistence, Hot/Warm/Cold residency, Quick Export, Production Export and immutable queue snapshots. Adjustment schema 15 is append-only and first permits IDs 26–28. No other public or private persistence envelope changes.

The spatial stack contract now reports cumulative X/Y radii. Dirty-region expansion, tile dependencies and cache fingerprints consume those anisotropic radii while the older scalar query remains available as a maximum-component compatibility wrapper. Large sampling footprints are bounded and stale renders observe cooperative cancellation.
