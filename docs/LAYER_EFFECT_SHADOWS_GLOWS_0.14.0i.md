# 0.14.0i — Shadows and Glows

0.14.0i activates the first concrete renderers on the distinct Layer Effect (`fx`) architecture established in 0.14.0h. These are not Live Filters and do not require a Smart Layer.

## Active effects

- Drop Shadow
- Inner Shadow
- Outer Glow
- Inner Glow

Each effect stores colour, independent opacity, blend mode, Size and Spread/Choke. Shadow effects also store Angle and Distance. Values are authored in document-space pixels and are rescaled only for the requested preview/tile resolution.

## Coverage and composition

Effect generation begins from the owning layer's effective full-precision silhouette, including its transformed layer mask. The owner content remains authoritative and unmodified.

- Drop Shadow: coverage → offset → spread → Gaussian soften → colourise → composite behind.
- Outer Glow: coverage → outward spread/soften → remove source interior → colourise → composite behind.
- Inner Shadow: shifted inverse coverage → choke/soften → intersect source coverage → colourise → composite above.
- Inner Glow: inverse coverage → choke/soften → intersect source coverage → colourise → composite above.

The generated pass retains its own blend mode and opacity. Layer opacity multiplies the generated pass at final composition, while the layer mask shapes the generating silhouette instead of clipping outward effects back to the source bounds.

## CPU/GPU contract

Coverage, Spread/Choke and Gaussian generation use one exact reference implementation. The WebGPU hierarchy consumes the same generated pass images as independent blendable/resident textures; it does not substitute a visually different blur. This preserves CPU/native parity while still accelerating composition and residency reuse.

## Tiling and caches

Spatial dependency radius includes Size, Spread/Choke, shadow offset and safety padding. Tiled dirty fingerprints hash the effect-expanded owner source/mask footprint. A 192 MiB bounded Layer Effect pass LRU allows unchanged effects to survive lower-layer composite invalidation, and native prepared passes can reuse the existing bounded VRAM residency cache.

Large full-resolution CPU renders with several enabled effects stream the generated passes one at a time instead of retaining every full-canvas pass simultaneously. Interactive/tiled regions continue to use the shared multi-pass cache so scrubbing and painting-underneath reuse are not sacrificed.

## Persistence

- `.vfxphoto`: 24
- Embedded Smart source schema: 7
- Hot/Warm/Cold snapshot: 25
- Layer Effect schema: 2

Schema-1 definitions migrate to deterministic effect defaults while remaining disabled. Stroke/Colour Overlay/Gradient Overlay/Bevel & Emboss remain explicit disabled definitions until their 0.14.0j/k renderers land.
