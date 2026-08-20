# Authoritative CPU LUT evaluator — 0.10.1c

## Evaluation order

For an input RGB triplet `x`, production now performs these stages in order:

1. Clip each component of `x` to its declared 1D shaper domain.
2. Sample the optional 1D table linearly and independently per channel.
3. Clip the shaped result to the declared per-channel 3D domain.
4. Address the red-fastest 3D lattice.
5. Sample it with the persisted trilinear or tetrahedral mode.
6. Blend the mapped value against the original unshaped input using Strength.
7. Return the scalar result without display-range clamping.

RGBA8/RGBA64 conversion remains the integer destination boundary and therefore clamps values that cannot be represented. Alpha is not part of LUT evaluation and remains unchanged.

## Interpolation

Trilinear interpolation remains available for compatibility. Tetrahedral interpolation partitions each lattice cell into six tetrahedra and evaluates one four-corner simplex. Exact equal-fraction cases use a fixed comparison order, avoiding platform-dependent changes in arithmetic order.

New imports use tetrahedral interpolation. Existing adjustment schemas 1–7 used only trilinear evaluation, so migration preserves that mode. Schema 8 writes the selected mode explicitly.

## GPU boundary

The current GPU table is RGBA8 and the WGSL sampler is trilinear. It is not treated as an approximation of tetrahedral CPU output. A tetrahedral layer therefore selects CPU compositing before GPU tile preparation. The startup diagnostic continues testing the legacy trilinear GPU path until 0.10.1f replaces it with floating-point storage and matching interpolation.
