# Specialised LUT operator profiles — 0.10.1e

## Why a named profile is necessary

A `.cube` table does not necessarily contain the complete transform used by the application that authored or distributed it. Tony McMapface and the supplied AgX Base sRGB table both require fixed operations before and/or after table sampling. Treating either file as an ordinary creative LUT sends it values from the wrong mathematical domain and produces an excessively strong or otherwise incorrect result.

0.10.1e stores the surrounding transform separately as `operatorProfile`. The table remains embedded in the adjustment layer as before.

## Generic .cube

`generic` preserves the 0.10.1d contract. The user chooses Encoded document values, Linear sRGB / Rec.709 or Raw component values, plus trilinear or tetrahedral interpolation. This is the default for ordinary LUTs and for every project migrated from adjustment schema 1–9.

## Tony McMapface

Persisted value: `tony-mc-mapface`.

For each pixel:

1. recognise the document as encoded sRGB, linear sRGB or unsupported preserved components;
2. decode recognised encoded sRGB to linear Rec.709;
3. calculate `encoded = linear / (linear + 1)` per channel;
4. apply any declared 1D shaper;
5. sample the 3D table tetrahedrally using its declared domain;
6. treat the table result as linear Rec.709;
7. encode back to sRGB when the document stores encoded sRGB;
8. blend Strength against the original stored RGB values.

The commonly distributed reference table is 48³. A different size is allowed but produces an Inspector warning because it cannot be assumed to be the reference table.

## AgX Base sRGB

Persisted value: `agx-base-srgb`.

Recognised encoded sRGB is first decoded to linear Rec.709. The supplied matrix is then applied using the same row-vector convention as the reference code:

```text
film.r = r*0.5594630473276861 + g*0.3047758110283366 + b*0.1358129414038276
film.g = r*0.0762332608733703 + g*0.7879523952184488 + b*0.1357748488287584
film.b = r*0.0655375095152927 + g*0.1645427298716744 + b*0.7697415276874705
```

Each positive FilmLight E-Gamut component is allocated into the table domain with:

```text
allocated = clamp((log2(component) - -12.47393) /
                  (12.5260688117 - -12.47393), 0, 1)
```

Non-positive values allocate to zero. The optional 1D shaper and 3D table follow, with tetrahedral interpolation forced. Each returned component is clamped to non-negative for the real-valued power operation and raised to `2.4`. The resulting linear Rec.709 values are encoded back to sRGB when required, then Strength blends in the document's stored component space.

The supplied reference table is 57³. Other dimensions are allowed with a warning.

## Detection and migration

Fresh imports normalise the source filename plus Cube `TITLE` by removing punctuation and spacing. Names containing `TonyMcMapface` suggest Tony; names containing both `AgXBase` and `sRGB` suggest AgX. This is only a convenience suggestion at import time.

Adjustment schema 10 persists the final choice. Schemas 1–9 always migrate to Generic, and project reopening never performs name-based inference. This prevents an old project from changing appearance merely because its embedded source name resembles a newly supported operator.

## Colour-space boundary

Both named profiles are defined for Rec.709/sRGB primaries. sRGB and linear-sRGB documents receive explicit transfer conversion. For a different ICC primary set, 0.10.1e preserves the stored components and displays a warning instead of guessing a gamut transform. Full assign/convert, display and output colour management remains 0.11.0 work.

The current integer document model also cannot restore scene-linear values already clipped before import. The implementation reproduces the reference mathematics for the values available to the document, but an already display-referred screenshot cannot recover lost HDR highlights.

## GPU boundary

Named profiles use the authoritative CPU evaluator. The current GPU LUT texture is RGBA8 and its WGSL path performs direct component sampling only; using it for these transforms would reintroduce the original correctness defect. 0.10.1f is reserved for floating-point lookup storage, tetrahedral WGSL interpolation and CPU/GPU conformance for the complete named pipelines.
