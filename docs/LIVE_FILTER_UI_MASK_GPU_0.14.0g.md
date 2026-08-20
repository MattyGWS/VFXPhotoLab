# 0.14.0g — Live Filter UI, Masks and GPU Integration

## Purpose

0.14.0g turns the persistent 0.14.0f Live Filter stack into a production-facing per-Smart-instance workflow while preserving the architectural order:

`Smart Source → instance transform → Live Filters → Smart layer mask → opacity/blend`

A Live Filter remains neither an Adjustment Layer nor a Layer Effect. The authoritative Smart Source does not absorb instance filter state.

## Layers and Inspector UX

Each Smart Layer exposes an **Add Live Filter** menu. Its Live Filters appear as dedicated child-like rows in the Layers panel with stable filter/owner roles, checkboxes and mask thumbnails. These rows are deliberately pseudo-rows: structural layer drag/drop, Merge, Duplicate, Transform and Convert-to-Smart operations cannot reinterpret them as `LayerNode`s.

Selecting a filter routes the normal Inspector adjustment editor through a Live Filter binding. The existing typed controls therefore edit the filter's `AdjustmentData` directly, including Levels, Curves, LUTs, colour controls and spatial filters. Slider/curve gestures use the existing grouped property-Undo contract.

## Per-filter masks

`LiveFilter` schema 2 adds an optional grayscale mask plus reference size/origin, enable state and inversion. The mask is stored in the Smart instance's reference space and follows that Smart Layer's transform/interpolation. CPU and native hierarchy paths request the same exact transformed mask.

Mask commands include:

- create from the current selection, or create white when there is no selection;
- enable/disable;
- invert;
- **Load Selection** to project the mask into document selection space;
- **From Selection** to commit an edited/feathered/refined selection back to the filter mask;
- remove.

The selection round-trip is intentionally the safe editable-mask workflow in 0.14.0g. A Live Filter pseudo-row is not made into a direct raster brush target, because the existing edit-target contract is layer/mask/channel-owned and should be extended explicitly rather than bypassed.

## Histogram and on-image analysis

`ImageProcessor::renderLiveFilterInput()` evaluates the selected Smart instance only through the filter prefix immediately before the target filter. It excludes the target/downstream filters, outer Smart mask, opacity/blend and unrelated parent layers. Levels/Curves histogram requests carry the Smart owner/filter IDs so the histogram service analyzes this exact input.

Existing Levels/Curves/Hue-Saturation/White-Balance on-image sampling remains available and commits parameter changes through the Live Filter binding.

## Native GPU integration

The WebGPU hierarchy compositor reuses the already parity-gated adjustment kernels. If every enabled filter in a Smart stack is approved by the startup parity gate, the Smart instance is represented as an isolated native subgroup: transformed Smart source at the bottom and ordered adjustment passes above it. Per-filter masks are transformed by the exact Smart-mask sampler and supplied to their native adjustment passes.

If any enabled operator is not approved, the stack uses the exact CPU Live Filter renderer. Its result still participates in the 0.14.0e Smart residency path and can be consumed by the native compositor. This is an explicit fallback, not a low-quality preview or silently disabled filter.

## Interaction and cache behavior

Live Filter edits preserve the 0.14.0f prefix-stage cache. Changing filter N leaves the transformed Smart source and unchanged prefix stages reusable. Existing spatial-interaction coalescing/detail-sensitive preview rules now detect selected spatial Live Filters as well as Adjustment Layers, preventing semantic source re-rendering on every slider pointer event.

## Persistence

- `.vfxphoto`: format **22**
- Embedded Smart document: schema **5**
- Hot/Warm/Cold snapshot: format **23**
- `LiveFilter`: schema **2**

Schema-1 Live Filters load with no mask. New mask metadata is rejected if it is injected into an older project, embedded-source or residency envelope. Runtime GPU/cache state is not serialized.

## Next boundary

0.14.0h begins the separate Layer Effect (`fx`) architecture. Live Filters remain Smart-only and instance-owned; 0.14.0h must not collapse that stack into Layer Effects merely because both systems can reuse cache/compositor infrastructure.
