# GPU backend — 0.11.0a

## 0.11.0a colour-state cache identity

The renderer still evaluates the same CPU/WGSL pixels as 0.10.1g.1. `RenderSessionContext` now carries the document colour-state revision; the backend rejects requests using an obsolete revision, and tiled composite revisions include it. This is an invalidation/stale-publication foundation only—0.11.0a adds no display, ICC or OCIO transform to the GPU path.

## 0.10.1g.1 LUT diagnostics and fallback reporting

The 0.10.1g.1 Inspector reports the same LUT eligibility contract used by native texture preparation. `CubeLut::gpuFallbackReason()` distinguishes half-float table overflow, unsafe f32 domains and packed texture limits; runtime status separately reports deliberate RGBA64 CPU processing, unavailable WebGPU/base parity and LUT-specific startup parity rejection. Eligible payloads are uploaded with `WGPUTextureFormat_RGBA16Float` and sampled with `textureLoad`; no sampler filtering or eight-bit intermediate texture is used. The shared lookup texture supports cube-only, shaper-only and combined files. Trilinear and tetrahedral interpolation are performed explicitly in WGSL, so the lookup representation is identical for both modes.

The native path now covers all persisted processing and operator choices introduced by 0.10.1d/e. The document transfer, generic processing mode and named profile are passed as uniform values. Tony and AgX force tetrahedral sampling in both CPU and GPU implementations. Alpha is never read from the LUT and remains governed by the existing alpha-safe adjustment/compositor contract.

At startup the helper process executes CPU/GPU parity cases for tetrahedral RGBA16Float, trilinear RGBA16Float, encoded/linear conversion, Tony McMapface and AgX Base sRGB. Every case maps to the single LUT adjustment feature bit. A failure in any case removes that bit and therefore sends LUT-containing hierarchies to the CPU reference while leaving approved non-LUT adjustments and all independent GPU capabilities enabled.

A payload remains CPU-only when a table sample exceeds finite binary16 range, an active domain cannot be represented as finite ordered f32, the packed texture exceeds the conservative 8192-pixel dimension limit, or native resource creation fails. RGBA64 documents remain CPU-authoritative. The native working tile is still RGBA8, so floating-point LUT output is retained through table processing and then quantised at the adjustment-tile write boundary.

The adjustment shader is no longer duplicated inside `WebGpuContext.cpp`. CMake embeds the exact checked-in `shaders/adjustment_tile.wgsl`, which is also the source inspected by `scripts/check-wgsl-reserved.py`.

The sections below preserve the staged implementation history. Statements about a then-current RGBA8 or CPU-only LUT path apply only to the named historical release, not to 0.10.1g.1.


## 0.10.1e named-operator eligibility

Tony McMapface and AgX Base sRGB include scene-linear preprocessing, fixed tetrahedral sampling and output operations that the current RGBA8 WGSL lookup kernel does not implement. `CubeLut::requiresCpuEvaluation()` therefore marks any non-generic operator profile for selective authoritative CPU evaluation, and `buildGpuTexture()` refuses to build a misleading lookup texture for it with an actionable fallback reason.

This fallback is adjustment-local. The compositor, brush paths, Fill, Gradient, resampling and other validated WGSL adjustments remain enabled. The operator profile is included in the tiled layer-tree cache hash so switching profiles cannot reuse a composite generated under another pipeline.

The startup diagnostic continues validating the unchanged generic trilinear LUT path. Floating-point lookup storage, tetrahedral WGSL sampling and CPU/GPU parity for named profiles are isolated to 0.10.1f.

## 0.10.1d transfer-aware LUT eligibility

The RGBA8 WGSL LUT kernel still samples stored components directly. 0.10.1d therefore does not pretend that it can perform encoded↔linear transfer conversion. `RenderBackend` now passes the document `QColorSpace` into LUT eligibility and asks `CubeLut::requiresCpuEvaluation()` before native hierarchy preparation.

The existing GPU path remains eligible for:

- Raw component mode on any document transfer;
- Encoded document mode on sRGB/untagged encoded documents;
- Linear sRGB / Rec.709 mode on linear-sRGB documents;
- valid non-sRGB ICC documents where the current milestone deliberately preserves and samples stored components without hidden conversion.

It selectively falls back for encoded mode on a linear-sRGB document, linear mode on an encoded sRGB document, tetrahedral 3D interpolation, extended-range table values or texture dimensions beyond the existing limit. This preserves the previous per-feature GPU gates and does not disable unrelated adjustments, Fill, Gradient, brush, Clone Stamp or compositor capabilities.

The startup LUT parity test remains a direct trilinear encoded-component case. It continues to validate the unchanged shader contract rather than claiming transfer-conversion parity. Floating-point table storage and matching tetrahedral WGSL remain 0.10.1f work.

## 0.10.1c LUT CPU-authority contract

New 3D and combined `.cube` imports use deterministic tetrahedral interpolation and are deliberately routed through the authoritative CPU evaluator. A 1D-only LUT remains a linear shaper and may still use the existing GPU lookup when otherwise compatible. `RenderBackend::compositorGpuReady()` rejects only hierarchies containing a tetrahedral LUT; unrelated adjustment, brush, Fill, Gradient and compositor capabilities remain GPU-enabled. Direct construction of a legacy LUT texture also fails cleanly with a tetrahedral-specific CPU-fallback reason.

The startup LUT parity diagnostic explicitly constructs a trilinear LUT. It continues to validate the existing RGBA8 WGSL lookup path, including its current 8-bit table quantisation, without implying tetrahedral support. Projects created by adjustment schemas 1–7 migrate to trilinear and can continue using this parity-approved path when their table values and dimensions are compatible.

The floating-point GPU table representation and tetrahedral WGSL evaluator remain scheduled for 0.10.1f. Until then, CPU evaluation is the source of truth for new imports, extended-range table values and every tetrahedral 3D LUT.

## 0.10.1b parser-stage backend history

## 0.10.1b parser-stage GPU contract

No shader, tile layout, compositor feature gate or native resource format changes in this stage. Parser/data-model conformance now preserves finite extended-range table values instead of clipping them. `gpuDisplayRangeCompatible` is recomputed from the preserved payload after import or persistence load; any sample outside `[0,1]` continues to disable only that LUT's RGBA8 lookup upload and uses the exact CPU adjustment path. In-range LUTs retain the existing startup CPU/WGSL parity gate.

The RGBA8 lookup quantisation remains the named baseline for 0.10.1f. Scalar output clamping was removed in 0.10.1c and 0.10.1d makes integer destination clamping explicit. Tiled rendering, 8-bit native compositing, 16-bit CPU reference processing, alpha/hidden-RGB preservation and all non-LUT GPU features are unchanged.

## 0.10.1a LUT precision baseline

No production GPU LUT path changes in this stage. The conformance suite explicitly demonstrates that `CubeLut::buildGpuTexture()` currently emits `QImage::Format_RGBA8888`, so table values are quantised before upload and cannot represent extended output. This is retained as a named known-defect baseline for the later floating-point GPU LUT stage. Existing tiled compositing, feature gates, CPU fallback, RGBA64 processing and all other native kernels are unchanged.

**New from Clipboard** performs no GPU work before the document is published. It prepares one validated straight raster using the existing CPU clipboard payload functions; normal session upload, tiling and residency begin only after `PhotoDocument` becomes active.

## 0.10.0l integration hardening

This stage does not add or modify a GPU kernel. The new safety preflight executes before any project payload can allocate renderer resources, request vector raster tiles or enter a residency snapshot. Excessive hierarchy, vector-node, guide or embedded-image data is therefore rejected while the document is still CPU-side and detached from the active renderer.

The existing per-feature startup gates remain authoritative: Fill and Gradient compare their 8-bit tiled WGSL application against the deterministic CPU reference and disable only the disagreeing feature; 16-bit targets remain on the exact CPU path. The integrated RGBA64 save/reopen regression verifies the committed CPU reference together with masks, selections and both merge types, while the Fedora plan repeats representative Fill/Gradient operations with native acceleration enabled and forced fallback. No GPU state, transient live-preview surface or device-dependent quantisation enters project format 14 or residency schema 15.

## 0.10.0k.1 merge editability note

The fix changes only UI selection and CPU-side vector object targeting. Activating a different object invalidates no document pixels; an actual object or layer-appearance edit advances the same vector revisions and clears the same vector raster cache as before. No shader, native tile, compositor, readback, alpha, 16-bit or fallback contract changes.

## 0.10.0k Merge Layers

Merge Layers does not add a new GPU kernel or alter the tiled compositor contract. Raster merge uses the exact alpha-safe CPU hierarchy reference to create one authoritative editable raster payload; normal document rendering resumes through the existing native compositor after the atomic tree replacement. Vector merge performs geometry-only path conversion and transform baking, then invalidates the vector raster cache so subsequent tiles are generated by the unchanged vector/compositor paths. No GPU-only state enters history, project files or residency snapshots.

## 0.10.0j.1 interactive Gradient preview

Live placement does not dispatch the full-resolution Gradient application kernel or perform GPU readback per pointer event. A coalesced worker evaluates the bounded CPU reference on a screen-resolution target and submits one interactive hierarchy composite for the corresponding reduced document surface. This keeps GPU queue/readback synchronisation out of the mouse path while still allowing the parity-approved native compositor to accelerate the detached preview hierarchy.

The interactive compositor is invoked off the UI thread under the existing render mutex and session-serial checks. If the hierarchy is not GPU eligible, the same worker performs the bounded CPU composite. Cancellation is checked before and after the target evaluation and throughout compositor preparation, so release or cancellation cannot leave a stale preview waiting behind the authoritative full-resolution operation. The final committed Gradient kernel, feature gate, tile quantisation and all CPU/GPU parity rules from 0.10.0j are unchanged.

## 0.10.0j tiled Gradient application

Gradient geometry and colours are evaluated in target-local pixel space. The global start/end coordinates plus each 256×256 tile origin are passed to `gradient_apply.wgsl`, so neighbouring tiles sample one continuous function rather than restarting at tile boundaries. The kernel supports raster RGBA, greyscale, single-component and mask targets, applies selection coverage, performs premultiplied raster feathering, explicitly quantises unorm output and preserves source RGB when the resulting Alpha is zero.

Startup validation exercises Linear raster RGBA, Radial mask, reversed Angle component, Reflected greyscale and Diamond Alpha-channel cases against `applyGradientCpu()`. Gradient receives its own feature gate: a difference above one 8-bit code value disables only Gradient acceleration. Allocation, shader, pipeline, dispatch or readback failure abandons the complete provisional native pass and reruns all affected tiles through CPU, preventing one operation from mixing GPU and CPU quantisation. RGBA64 always uses the exact 16-bit CPU path.

Live dragging intentionally uses the bounded CPU preview reference because it operates on the document preview tier and must respond without synchronising full-resolution GPU readback on every pointer event. Release alone executes the authoritative tiled path, records one history delta and advances the document session serial. No GPU-only Gradient state survives release, cancellation, device loss or residency eviction.

## 0.10.0i.1 Fill shader validation hardening

The Fill WGSL uses complete-vector assignment for RGB updates. The Python pre-build validator scans both `shaders/*.wgsl` and embedded `R"WGSL(...)WGSL"` blocks for unsupported multi-component swizzle assignments, while `createShaderModule()` performs the same defensive check at runtime. This prevents the invalid shader-module, pipeline, bind-group and queue-submit chain that previously crashed the isolated diagnostic and forced the main application onto CPU fallback.

## 0.10.0i tiled Fill application

Tolerance matching and contiguous/global region discovery remain deterministic CPU work because they require ordered connectivity and exact agreement across devices. The resulting 8-bit coverage mask bounds the dirty region. `TiledCanvasEngine` applies that mask in 256×256 tiles; RGBA8 and mask tiles may use `fill_apply.wgsl`, which keeps storage in straight unorm components, blends partial raster coverage in premultiplied space, quantises explicitly to one 8-bit code value and preserves raster Alpha when requested. A feature-specific startup parity case compares the kernel against `applyFillCoverageCpu()` and disables only Fill acceleration on disagreement. GPU allocation, pipeline, dispatch or readback failure discards the provisional native pass and reruns the complete bounded operation through the exact CPU reference, so one committed Fill never mixes GPU-quantised and CPU-exact tiles.

RGBA64 remains CPU-only; scalar/channel work uses 16-bit component interpolation while partial raster coverage uses the same premultiplied alpha-safe blend as the 8-bit path. No matching state, selection state or document mutation exists only on the GPU. The completed target invalidates the existing layer surface/session generation exactly once, so stale composite tiles cannot publish after the atomic Fill operation.


## 0.10.0h preset note

Vector appearance presets store and apply semantic CPU-side style data only. Applying a preset invalidates the same vector revisions and requested tiles as Inspector or Copy/Paste Appearance edits, after which rasterised straight-RGBA tiles enter the unchanged native WebGPU hierarchy compositor. No GPU resource, shader, readback, tile-cache, hidden-RGB, 16-bit reference or CPU-fallback contract changes in this stage.


## 0.10.0g.2 shaft-clipping note

The arrowhead correction remains a bounded CPU vector-geometry operation before normal tile rasterisation. Restricting cap removal to the endpoint footprint changes no GPU resource, dispatch, cache, compositing, alpha or fallback contract; it only prevents the CPU coverage path from dropping unrelated winding-path segments before tiles are requested.

## 0.10.0g.1 arrowhead note

Arrowheads and semantic Arrow shapes remain vector geometry. Their centred endpoint-marker coverage participates in the same vector-layer tile bounds, CPU reference rasterisation, GPU compositing, masks and progressive presentation as existing shapes. No new raster staging path or alpha representation was introduced; Expand Stroke remains a bounded CPU geometry operation and the resulting filled paths return to the normal tiled renderer.


Bézier anchors, handles and live-corner radius/style fields remain semantic CPU-side project data. Cornered outline construction, curve construction, stroke/fill coverage and antialiasing use the deterministic requested-region vector rasteriser. Resulting straight-RGBA tiles enter the existing native WebGPU hierarchy for masks, opacity, blend modes, adjustments and Isolated/Pass Through groups.

No GPU-only path representation, tessellation cache or duplicate vector scene graph is introduced. GPU capability validation and selective fallback remain unchanged. RGBA64/16-bit documents use the exact CPU reference compositor, and cancellation/session serial checks prevent stale path tiles from being published after edits or document switches.

Pen endpoint markers are canvas-only vector overlays measured in screen pixels. Hovering Continue/Close/Join targets performs no rasterisation and schedules no native GPU work. Adding, closing or joining paths invalidates the same semantic vector revisions and requested tiles used by ordinary node editing; transformed donor geometry is resolved once on the CPU before the existing tiled rasteriser/compositor pipeline runs. Escape cancellation restores the captured structural state and retires any obsolete render serial before stale tiles can publish.

## Interactive publication

Continuous path gestures use the same ordered interactive-publication contract as adjustment drags: one visible level-0 request is allowed to finish and publish while newer pointer values are coalesced. In-flight native readback is not repeatedly cancelled, preventing high-rate mouse input from starving the display. When the gesture ends, only the exact current generation may publish.

Transformed `QPainterPath` geometry and stroke outlines are cached on the CPU and reused by every tile in a generation before those straight-RGBA tiles enter the native hierarchy compositor. Pure translation uses the already sharp foreground surface in the canvas and therefore requires no GPU dispatch during pointer movement; the authoritative semantic GPU/CPU render is published after release. No curve quality, antialiasing, Alpha precision or fallback behaviour is reduced.

## Live-corner interaction

Corner drags use the existing known-changed interactive mutation and ordered preview-publication paths. Only selected node radius/style fields change at pointer rate; generated `QPainterPath` geometry is fingerprinted by those fields and invalidates the transformed-geometry cache deterministically. Baking is an explicit CPU semantic conversion and does not alter GPU compositing contracts.

## Keyboard node editing

Keyboard node movement changes only semantic CPU-side path coordinates. The document-pixel delta is mapped to path-local space once, all selected anchors and handles are updated atomically, and the vector revision invalidates the same bounded tiles used by pointer editing. No GPU-side node buffer or alternate tessellation path is introduced. GPU/CPU compositing, RGBA64 reference behaviour, cancellation serials and hidden-RGB preservation remain unchanged.

## 0.10.0d appearance and interaction note

Vector appearance quick actions modify semantic CPU-side style data and continue through the existing deterministic vector rasteriser and tiled compositor. They add no new GPU resources or readback paths. The middle-button fix changes only canvas event priority; tiled presentation, residency and preview publication are unchanged.
## 0.10.0e hover feedback note

Vector hover and selection feedback is a canvas-only overlay. Hit testing uses existing semantic CPU path data and the current document-to-viewport transform; painting occurs after the tiled image presentation and does not invalidate vector revisions, request GPU tiles, change alpha/hidden RGB or enter the CPU reference compositor. Segment highlights carry a transient document-space `QPainterPath` solely for viewport drawing.


## 0.10.0f.1 Expand Stroke contour note

Expand Stroke is an explicit CPU semantic conversion, not a new rendering backend. It reuses the same `QPainterPathStroker` and alignment clipping/subtraction geometry already cached for vector tiles, then persists each disconnected island or hole boundary as an ordinary closed contour in one compound filled path. No synthetic connectors are emitted. Subsequent rendering combines those contours with the persisted path fill rule—nonzero for generated stroke outlines—and follows the unchanged requested-region vector rasteriser and native tiled hierarchy compositor. Isolated fill/stroke child groups preserve one application of masks, opacity and blend modes; RGBA64 documents remain on the exact CPU reference path and no hidden RGB or Alpha data is rewritten.

## 0.10.0f.2 dashed expansion note

The dashed-stroke hotfix changes only vector outline extraction and validation before ordinary vector publication. It does not alter the tiled WebGPU renderer, residency, raster compositing, CPU fallback or 8/16-bit pixel paths. Expanded results continue through the existing vector geometry cache and rasterizer.

## 0.10.0f.3 winding note

Expand Stroke remains a vector/model operation. The correction changes only compound path fill semantics from forced even-odd to persisted nonzero winding for generated outlines. GPU tiled rasterisation receives the resulting path through the same vector coverage interface; no GPU/CPU pixel-processing contract changes.
