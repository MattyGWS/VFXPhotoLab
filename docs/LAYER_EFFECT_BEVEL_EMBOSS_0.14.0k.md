# 0.14.0k — Bevel & Emboss

0.14.0k activates the final core Layer Effect renderer planned for the 0.14.0 milestone: **Bevel & Emboss**. It remains authored in the independent per-layer `fx` stack and is not a Live Filter or Adjustment Layer.

## Authored controls

Core styles are **Inner Bevel**, **Outer Bevel**, **Emboss** and **Pillow Emboss**, with **Up/Down** direction, Depth, Size, Soften, light Angle and Altitude, plus independent Highlight and Shadow colour/blend/opacity controls. Rich gloss contours, texture/pattern variants and specialised contour libraries are intentionally left to 0.17.0.

## Coverage and distance geometry

The renderer starts from the same full-precision effective coverage contract used by the other Layer Effects. That coverage already reflects the rendered owner content and transformed layer mask while remaining independent from final layer opacity/blend. A two-pass Euclidean squared-distance transform finds distance to the inside and outside coverage classes. Those distances form a signed boundary-distance field.

Each Bevel style maps signed distance into a bounded height profile. Direction can invert that profile. Soften applies the existing exact Gaussian reference to the encoded height field. Finite-difference X/Y derivatives then form a local surface normal, which is lit by the authored Angle and Altitude. Depth controls the height-gradient strength used by the normal.

The lighting result is emitted as two independent generated passes: Highlight and Shadow. Each keeps its own colour, blend mode and opacity, allowing native hierarchy composition to blend the passes against the real backdrop instead of flattening them into source content.

## Tiling, cache and memory

Bevel's dependency radius is Size + Soften + the spatial safety padding. Tiled requests expand coverage by that halo before evaluating the signed-distance/normal field, so independently rendered tiles stitch to the same pixels as a full-region request. Generated passes reuse the existing bounded Layer Effect intermediate cache and native resident-texture path.

Large full-resolution CPU renders use a 512×512 streaming path for Bevel. Each streamed tile still receives its complete effect halo, while distance fields, height data and Highlight/Shadow passes remain bounded instead of multiplying into several full-document allocations.

## GPU contract

The coverage/distance/lighting implementation is the exact shared CPU reference in 0.14.0k. WebGPU accelerates the existing independent pass residency/composition with the authored blend modes. There is no separate approximate GPU Bevel kernel; therefore CPU/native hierarchy results share the same generated effect pixels.

## Persistence

0.14.0k uses public project format **26**, embedded Smart document schema **9**, private Hot/Warm/Cold snapshot format **27**, and Layer Effect schema **4**. Schema-3 Bevel definitions migrate disabled with deterministic current defaults. Older envelopes reject schema-4 fields, including when nested inside embedded Smart Sources.
