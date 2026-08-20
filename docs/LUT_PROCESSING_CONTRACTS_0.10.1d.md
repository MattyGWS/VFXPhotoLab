# LUT input and output semantics — 0.10.1d

## Purpose

A `.cube` file contains table dimensions, domains and samples, but it does not reliably identify the colour encoding expected before the table or after it. Earlier VFX Photo Lab builds always sampled the document's stored RGB values directly. That behaviour was deterministic, but it was implicit and wrong for LUTs authored for a different transfer domain.

0.10.1d makes that surrounding contract explicit without attempting to replace the full colour-management milestone.

## Persisted processing modes

### Encoded document values

This is the compatibility and new-import default.

- On an sRGB or untagged encoded document, stored values are sampled directly.
- On a linear-sRGB document, values are encoded with the extended sRGB transfer function before the LUT and decoded afterwards.
- Strength blends the returned result in the document's stored component space.

This mode is appropriate for ordinary creative LUTs authored for conventional encoded image values.

### Linear sRGB / Rec.709

The table is treated as operating on linear-light sRGB/Rec.709 primaries.

- On an sRGB encoded document, values are decoded before the LUT and encoded afterwards.
- On a linear-sRGB document, stored values are sampled directly.
- Strength blends after conversion back to the document's stored component space.

sRGB and Rec.709 share primaries; this stage uses the sRGB transfer pair for encoded document conversion. Operator-specific logarithmic encodings and gamut transforms are not inferred from the `.cube` file.

### Raw component values

The table receives the stored RGB components exactly as they are. No transfer-function or gamut conversion occurs before or after the table. This is useful for deliberately numeric LUTs, log data managed externally, and diagnostics.

## Extended values

The transfer functions are extended rather than display-clamped:

- negative encoded values remain on the linear segment;
- negative linear values encode through the linear segment;
- values above one remain above one through the power segment;
- table outputs remain unclamped during Strength blending.

RGBA8 and RGBA64 paths clamp explicitly only when writing the final integer pixel. This avoids wraparound while preserving the scalar evaluator as the future float/HDR reference.

## Document profiles

The current document model recognises sRGB primaries paired with either the sRGB or linear transfer function, including equivalent ICC profiles reconstructed by Qt rather than only its built-in named instances. For a different valid ICC profile, 0.10.1d does not guess a gamut transform or silently convert through sRGB. The stored components are preserved and sampled directly, and the Inspector explains that limitation for Encoded and Linear modes.

Full input, working, display and output profile conversion remains scheduled for 0.11.0.

## Migration

Adjustment schema 9 adds `processingMode`:

- schemas 1–8 migrate to `encoded-document`;
- schemas 1–7 also migrate to `trilinear` interpolation;
- schema 8 retains its persisted interpolation and gains the encoded processing default;
- project format remains 14;
- private Hot/Warm/Cold snapshot schema remains 15.

## GPU contract

The current WGSL LUT path performs direct component sampling using the existing RGBA8 lookup texture. It remains valid only where the selected processing mode requires no transfer conversion for the current document profile.

Selective CPU fallback is used for:

- Encoded mode on a linear-sRGB document;
- Linear mode on an encoded sRGB document;
- tetrahedral 3D interpolation;
- extended-range table samples;
- lookup dimensions beyond the current native texture limit.

Raw mode, encoded mode on encoded documents and linear mode on linear documents can retain the validated native path when all other LUT constraints pass.

## Deliberate remaining limitation

Tony McMapface and AgX Base sRGB may still not match their reference renderer after choosing a more appropriate processing mode. Their reference code contains operator-specific preprocessing and postprocessing that is not represented by the table alone. Those named pipelines are 0.10.1e work.
