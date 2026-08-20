# 0.13.0e — Optical and Spatial Colour Effects

This stage adds three ordinary non-destructive adjustment layers while retaining the existing project, residency, preset, production-export, queue and recovery envelopes.

## Vignette

Vignette is evaluated relative to current document bounds and supports Amount, Midpoint, Roundness, Feather, Centre X/Y, Rotation, Highlight protection, optional colour/tint and an inverted edge/centre mask. Positive Amount darkens; negative Amount brightens. It uses the managed encoded-sRGB adjustment domain and preserves straight Alpha and hidden RGB.

## Creative RGB Split

Red and Blue sample independently from signed document-pixel offsets while Green and Alpha remain anchored. Sampling is bilinear with clamped edges. The maximum offset plus one bilinear safety pixel feeds the radius-aware tiled halo contract.

## Manual radial chromatic-aberration correction

Signed Red and Blue edge shifts are applied radially around an adjustable optical centre. Falloff controls how quickly displacement grows from the centre to the furthest document edge. This is a manual creative/corrective operator and does not consume camera or lens profiles; profile-driven correction remains 0.15.0.

## Compatibility

Adjustment JSON advances to schema 14 and appends stable identifiers 23–25. All earlier identifiers and schemas retain their previous meaning. Project format 15, Hot/Warm/Cold schema 16, colour-state schema 4, vector schema 7, preset envelopes, export profiles, production plans, queue descriptions and recovery records are unchanged. The new effects use exact multicore CPU references until dedicated WGSL implementations pass the existing per-adjustment parity gate.
