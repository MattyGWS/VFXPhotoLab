# 0.14.0f — Live Filter Stack Foundation

## Scope

0.14.0f establishes the persistent/renderable Live Filter stack on Smart Layer **instances**. It deliberately stops before the user-facing Live Filter editor, per-filter masks and native per-filter WebGPU kernels planned for 0.14.0g.

## Ownership and semantics

A Live Filter belongs to a Smart Layer instance, not to its Smart Source. Two Smart instances may share one authoritative source while carrying completely independent filter stacks.

The render order is:

`Smart Source → Smart instance transform → Live Filters → Smart instance mask → opacity/blend`

This is intentionally different from an Adjustment Layer, which affects composition below it, and from Layer Effects (`fx`), which will generate/composite appearance from layer coverage beginning in 0.14.0h.

Each entry stores:

- stable UUID;
- enabled state;
- monotonic entry revision;
- typed `AdjustmentData` payload;
- Live Filter schema version.

All existing 0.13 adjustment/filter operator types are representable as Live Filters. Their existing parameter normalization, 8/16-bit CPU behavior and colour-processing contracts remain authoritative.

## CPU stack and halos

The exact CPU path starts from the cached, transformed Smart presentation provided by 0.14.0e and evaluates enabled entries in order. Spatial operators contribute X/Y dependency radii; the stack requests their cumulative parent-space halo before evaluation and crops back to the requested output region afterward.

The dependency rectangle is allowed to extend outside the parent canvas. This is required for off-canvas Smart content to blur/filter back into visible pixels and avoids silently treating the document edge as a layer-content edge.

## Intermediate cache contract

0.14.0f reuses `SmartLayerTileCache` rather than creating a second cache subsystem. Each enabled stage is keyed from:

- the exact preceding stage image identity;
- normalized typed filter parameters;
- requested expanded region;
- preview/document geometry;
- colour-processing compatibility.

The stable filter UUID/revision is **not** part of the pixel-stage key. This is deliberate: a disable/re-enable or Undo path with identical input and parameters may reuse an exact previous result. Structural identity remains in persistence/Undo state, while pixel cache identity follows rendering semantics.

Changing filter N therefore invalidates N and downstream results while leaving the transformed source and unchanged earlier prefix stages reusable. Editing content underneath the Smart Layer can reuse the entire unchanged Live Filter chain.

Final tiled-composite hashes and native Smart resident keys include enabled filter semantics so a changed visible stack can never borrow stale parent/GPU pixels. Parameter edits to a disabled filter are intentionally non-contributing until it is enabled.

## GPU boundary in 0.14.0f

Native hierarchy composition receives the exact CPU-evaluated Live Filter tile and may cache/reuse that tile through the existing WebGPU Smart residency path. This is an honest CPU fallback, not a fake low-quality preview.

0.14.0g remains responsible for native tiled GPU execution of supported filters, UI parameter scrubbing, per-filter masks and GPU/CPU parity coverage for those paths.

## Persistence

- Public `.vfxphoto`: format **21**.
- Embedded Smart Source document: schema **4**.
- Private Hot/Warm/Cold snapshot: format **22**.
- Live Filter entry: schema **1**.

Pre-21 projects and pre-schema-4 embedded source documents migrate with empty Live Filter stacks. Loaders reject Live Filter metadata placed in an older envelope rather than silently interpreting incompatible state.

## Memory and safety

A Smart Layer is limited to 64 Live Filter entries. Layer-tree safety validation rejects duplicate/null/unsafe entries. Dynamically allocated payloads such as LUT tables, curve points and gradient stops are included in document residency/Undo memory estimates.

Runtime Live Filter intermediates remain bounded by the 0.14.0e Smart cache budget and are never serialized as authoritative state.
