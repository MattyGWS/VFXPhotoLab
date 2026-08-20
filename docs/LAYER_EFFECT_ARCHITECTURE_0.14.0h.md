# 0.14.0h — Layer Effects Architecture and `fx` UX

Layer Effects are authored per-layer appearance and are deliberately distinct from Live Filters and Adjustment Layers. Raster, Vector, Text and Smart Layers own ordered `LayerEffect` definitions.

The render contract is:

`Layer content / Smart Source + Transform + Live Filters` → `Layer Effects` → `Layer Mask` → `Opacity / Blend`

Effect generation also receives a separate **mask-aware coverage side input** so effects such as shadows and glows can derive from the effective visible silhouette without destructively baking the mask into authoritative content. `renderLayerEffectInputRegion()` returns unclipped hidden-RGB-preserving content plus grayscale 8/16-bit coverage.

Schema-1 definitions intentionally contain identity/type/enabled/revision only. Because no concrete effect renderer exists in 0.14.0h, definitions are created disabled and an attempt to enable one is rejected with the exact implementation revision. 0.14.0i-k extend this durable contract with typed parameters and renderers rather than replacing it.

The Layers panel represents effects with presentation-only `fx` rows. These rows are not `LayerNode`s and cannot enter structural drag/drop, merge, duplicate or transform operations.
