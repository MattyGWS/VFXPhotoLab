# 0.13.0g.15a — Vector Feather Data Model and Inspector Foundation

## Scope

This revision begins the five-part non-destructive vector Feather sequence. It adds editable per-layer state, Inspector interaction, history, persistence and copy/merge rules only. It does not implement the final CPU or GPU feather equation and does not begin Smart Layers or Live Filters.

## Data model

`VectorLayerData` now owns:

```cpp
double featherRadius = 0.0;
```

The value is measured in document pixels and applies once to the complete vector layer rather than separately to every contained shape. This is important for converted paths, compound paths, imported SVG layers and merged vector layers, all of which may contain several editable objects but still represent one layer-level appearance.

Vector schema 8 writes `featherRadius` explicitly. Schemas 1–7 migrate to `0.0 px`. Schema-8 values must be finite and within `0.0…1,000,000.0 px`; malformed values are rejected. The loader does not silently clamp persisted data.

Text layers use `TextLayerData`, not `VectorLayerData`, so this revision does not expose Feather for text.

## Inspector and Undo

Every vector layer, including an empty vector layer, receives a **Vector layer appearance** section containing:

```text
Feather    0.0 px
```

The established `SliderSpinBox` provides typed entry and horizontal scrubbing. The field uses a `0.1 px` step and one decimal place. Normal dragging is practical near small values, Shift-drag provides tenths, and exact typed values remain available across the persisted safety range.

A scrub gesture or completed typed edit captures one property-Undo operation. Live changes update the layer, its revision/cache fingerprint, preview scheduling and document state without rebuilding the Inspector on every pointer event.

## Persistence and copies

The value travels through the existing vector JSON envelope used by:

- `.vfxphoto` project save/load;
- private Hot/Warm/Cold session backing snapshots;
- editable vector clipboard copies;
- ordinary layer duplication;
- inserted vector-layer copies.

Public `.vfxphoto` projects advance from version 15 to 16 and private Hot/Warm/Cold snapshots advance from version 16 to 17 because this is a new vector-layer capability. Project versions 1–15 and snapshot versions 2–16 remain readable. Legacy vector schemas migrate to `0.0 px`; a non-zero Feather value paired with an older outer envelope is rejected rather than silently accepted under a false version. Export-plan, queue and recovery versions are unchanged.

## Merge and presets

Editable vector merge can represent only one layer-level Feather value. This revision therefore preserves Feather when all selected vector layers match and rejects a mixed-value merge with an explicit message. It does not silently select the top/bottom/maximum value and does not rasterise.

Existing vector appearance presets remain fill/stroke style payloads. Applying a preset does not reset or overwrite the independent Feather value.

## Rendering boundary

The accepted vector rasterizer remains unchanged in 0.13.0g.15a. Feather participates in vector and tiled cache fingerprints so state changes invalidate the correct render identity, but actual softened coverage is deliberately deferred to 0.13.0g.15b.

Consequently:

- `0.0 px` remains pixel-identical to 0.13.0g.14.1;
- geometry, fills, strokes, gradients, holes, arrowheads, masks, Alpha-safe hidden RGB and 8/16-bit behaviour are untouched;
- non-zero values are persisted and editable but are not yet claimed as the finished visual renderer.

## Next focused revision

0.13.0g.15b will rasterise combined fill/stroke coverage, feather that coverage exactly on CPU and composite the original vector appearance through the softened boundary while preserving hidden RGB and Alpha-safe behaviour.

> **15d integration note:** the provisional 15a same-value Merge rule was superseded in 0.13.0g.15d. Once the exact renderer existed, it became clear that two independently feathered layers cannot generally be collapsed into one layer-level Feather operation over the merged silhouette, even at equal radii. Current builds therefore reject every non-zero editable vector merge rather than changing appearance.
