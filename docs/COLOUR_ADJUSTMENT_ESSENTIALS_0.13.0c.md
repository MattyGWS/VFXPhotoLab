# 0.13.0c — Colour Adjustment Essentials

0.13.0c completes the planned essential colour-adjustment set without changing the public project container, Hot/Warm/Cold snapshot envelope, colour-management state, vector payload, export profile, production-plan, queue or recovery schemas.

The accepted 0.13.0b.4 tree already contained public Vibrance, Threshold and Posterise adjustments. This stage preserves their established identifiers and rendering behaviour, adds the missing Invert and Photo Filter operations, and expands deterministic coverage across all five essentials.

## Stable adjustment identities

Adjustment identifiers remain append-only:

- `Vibrance` remains 6.
- `Posterise` remains 12.
- `Threshold` remains 13.
- `GaussianBlur` through `HighPass` remain 16–19.
- `Invert` is appended as 20.
- `PhotoFilter` is appended as 21.

Adjustment JSON advances from schema 11 to schema 12 only so identifiers 20 and 21 can be represented honestly. Schemas 1–11 retain their historical interpretation. A payload that labels Invert or Photo Filter as schema 11 or earlier is rejected instead of silently reinterpreted.

## Invert

Invert is an ordinary non-destructive adjustment layer with no operation-specific parameters. It computes `1 - component` independently for straight Red, Green and Blue code values. Alpha is copied exactly, including at zero coverage, so hidden RGB remains editable and export-safe. Layer opacity, blend mode, masks, groups, Pass Through compositing and adjustment presets continue through the generic adjustment-layer paths.

The operator runs in the document's encoded working-component domain. It does not introduce an ICC, OpenColorIO, ACES, display, proof or export-output conversion.

## Photo Filter

Photo Filter persists three values:

- filter colour;
- Density from 0–100%;
- Preserve luminosity.

The selected colour is interpreted as encoded sRGB, converted to linear-light Rec.709, normalised to a bounded optical transmission scale and applied to linear RGB. Preserve luminosity normalises that scale by Rec.709 luminance before Density is applied. The result is gamut-compressed from the source colour toward the filtered target, encoded back to sRGB and returned through the existing managed adjustment-domain transform when the document working space differs.

This makes the operation independent of monitor ICC, soft proofing and output-export transforms while preserving the established managed-working-space contract. Alpha and hidden RGB are never filtered.

The Inspector exposes standard Warming 85/LBA/81, Cooling 80/LBB/82 and Sepia colours, a custom colour picker, Density and Preserve luminosity. The same named looks are embedded as self-contained adjustment presets so projects never depend on future UI labels.

## GPU and CPU parity

WGSL identifiers 20 and 21 mirror the CPU contracts. Invert is evaluated directly. Photo Filter mirrors the same transfer functions, luminance coefficients, transmission scale, Density exponent and gamut-limiting interpolation.

Both operations enter the existing per-adjustment runtime parity suite. Photo Filter is also included in the Display-P3 managed-domain parity suite. A failed feature approval disables only that operation's WGSL bit; the exact CPU reference remains available. Spatial-filter approval and 0.13.0b interaction behaviour are unaffected.

## Persistence and production integration

The generic adjustment payload carries the new operations through:

- `.vfxphoto` save/load;
- Hot/Warm/Cold private snapshots;
- multi-document isolation;
- adjustment preset import/export;
- Quick Export and Production Export;
- immutable queue snapshots and recovery descriptions;
- Undo/history and shutdown preservation.

Project format remains 15, Hot/Warm/Cold schema remains 16, colour-state schema remains 4 and vector schema remains 7. Existing files are not rewritten until an explicit user save or preset update.

## Deterministic coverage

The stage adds tests for append-only identities, schema-12 round trips, dishonest legacy-schema rejection, 8-bit and 16-bit Invert, zero-Alpha hidden RGB, Photo Filter identity at zero Density, warm/cool direction, luminosity preservation, full-frame versus region rendering, built-in presets, WGSL publication and Cold snapshot restoration.
