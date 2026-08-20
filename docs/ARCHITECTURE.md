# VFX Photo Lab architecture — 0.13.0g

## Preset management boundary

`PresetManagerDialog` is a presentation adapter over the feature stores; it does not parse or serialise payloads itself. `PresetManagerCallbacks` supplies load/apply/create/update/rename/duplicate/metadata/favourite/delete/import/export operations, allowing adjustment and vector presets to share search, filtering, details and file-exchange behaviour while retaining type-specific validation and application semantics.

The compact adjustment Inspector combo and vector Appearance menu remain fast paths. The manager is opened only for advanced organisation. Applying an adjustment preset targets the layer UUID and expected adjustment type captured when the manager opened and creates one Undo entry. Applying a vector appearance uses the existing selected-vector operation or active Shape/Pen defaults. Save/update callbacks read current state when invoked rather than retaining a stale opening snapshot.

Immutable built-in definitions cannot carry user-specific favourite/recent values. `PresetUsageStore` therefore owns a bounded atomic sidecar keyed by stable preset ID. This sidecar contains favourite, last-used and use-count data only. It is application-global and excluded from project/snapshot schemas, Undo, document modified state, render identity and output pixels.

Search covers display name, category and tags. Source, category, favourite and recent filters are view state only. A malformed preset remains isolated by its feature store; a failed manager operation leaves existing preset files and document state unchanged.

## Unified preset boundary

`PresetCore` owns the cross-feature file envelope and metadata contract. Payload interpretation remains with feature adapters: `AdjustmentPresetStore` validates `AdjustmentData`, while `VectorAppearancePresetStore` validates `VectorAppearance`. A future export-profile adapter will use the same envelope without teaching the generic core about rendering or file formats.

The version-2 envelope contains `format`, `version`, `kind`, `metadata` and `payload`. Metadata supplies a stable ID, display name, category, tags, favourite state and bounded usage timestamps/counters. Built-ins use deterministic IDs and are generated in code; user presets use UUID-backed IDs and platform app-data storage.

Legacy adjustment version-1 files and vector-appearance version-1 files are read by their original adapters. Listing never rewrites them. An explicit user mutation first writes a validated version-2 replacement with `QSaveFile`, then removes the legacy file; failure to remove the old file rolls back the new file. This avoids silent reinterpretation and avoids half-migrated state.

Preset storage is application-global, not document state. No preset metadata is added to `.vfxphoto` projects or Hot/Warm/Cold snapshots in 0.12.0a–c. Applying a preset changes only the same adjustment/vector state it changed previously; recent-use bookkeeping is a separate app-data write and never enters Undo, rendering caches or document modified state.


## 0.11.0d adjustment-domain boundary

`AdjustmentProcessingDomain` gives every adjustment an explicit mathematical contract: encoded working space, linear working space, encoded sRGB/Rec.709, raw components, or the specialised LUT contract. Managed CPU rendering enters the declared domain through the central `ColourTransformService`, evaluates the existing operator, and converts the result back to the authoritative document working space. Legacy V1 bypasses this adapter and therefore remains byte-compatible with the historical adjustment path.

The processing-compatibility value travels with render-session, preview, histogram, export, merge and trim requests. In 0.11.0g, managed 8-bit stacks can remain on the native compositor: paired reference-baked working↔domain lattices surround linear-working and sRGB/Rec.709-domain WGSL operators. A profile, lattice or runtime parity failure still selects the exact CPU compositor for only the affected managed stack. Raster-only and encoded/raw-contract stacks retain their existing native path.

## 0.11.0c profile-operation boundary

`ColourConversion` prepares Assign and Convert operations without mutating a live `PhotoDocument`. Assign retags straight raster payloads and updates `DocumentColourState`; Convert obtains one cached Qt ICC transform and reuses it for RGBA8/RGBA16 raster payloads and semantic vector, text and Gradient Map colours while preserving Alpha and scalar coverage. `PhotoDocument::replaceStructuralState` atomically commits pixels, layers, selection, guides, resolution and colour state, assigning a fresh monotonic colour revision for initial commit and Undo/Redo. `MainWindow` runs conversion asynchronously, gates other mutations, supports cancellation, rejects stale results and records one structural history command.


## 0.11.0b ICC ingress boundary

`ImageProfileImport` performs bounded container inspection for PNG, JPEG and TIFF profile declarations, reconciles that evidence with Qt's decoded `QColorSpace`, and produces `ImageColourImportInfo`. `MainWindow` resolves the application-wide untagged policy before a `PhotoDocument` is created. The resolver may attach an sRGB interpretation or leave the image untagged. File open and New from Clipboard never convert component values; Paste into an existing differently tagged document continues through the pre-existing clipboard conversion path. `DocumentColourState` schema 2 persists origin status, the applied policy and the original profile fingerprint through public projects and private residency snapshots. See `COLOUR_MANAGEMENT_IMPORT_0.11.0b.md`.

## 0.11.0a explicit colour-state boundary

`PhotoDocument` owns a `DocumentColourState` independently of the `QImage` tag. The tag remains the compatibility carrier used by current rendering, while the state records separate input, working, display, proof and output roles. No new transform is applied in 0.11.0a. Project versions 1–14 and snapshot versions 2–15 migrate to `LegacyV1`; new documents record `ManagedV1`. A central cached transform service and colour-state-aware render/histogram/thumbnail revisions establish the foundation for later ICC/OCIO stages. See `COLOUR_MANAGEMENT_FOUNDATION_0.11.0a.md`.

## 0.10.1g.1 LUT integration and diagnostics architecture

`CubeLut::buildGpuTextureData()` converts the validated table payload directly to a tightly defined `LutGpuTextureData` object. Cube entries use the same red-fastest addressing as the CPU evaluator (`x = red + blue * size`, `y = green`); an optional 1D shaper occupies one final row. Each texel is RGBA16Float with an unused Alpha value of one. No `QImage`, colour conversion or display-range quantisation participates in the table upload.

The native adjustment uniform now carries interpolation, processing mode, operator profile and document transfer separately from Strength and table dimensions. `adjustment_tile.wgsl` implements the same ordered stages as `CubeLut::evaluate()`: document-transfer interpretation, optional named preprocessing, 1D shaper, 3D table, deterministic interpolation, optional named output operation, return to document component space and Strength blending. Tetrahedral branches retain the CPU evaluator's exact tie ordering.

Generic tables, Tony McMapface and AgX Base sRGB all share the same floating-point texture transport. Encoded/linear conversions use the extended sRGB transfer functions already defined by the CPU reference. Unsupported ICC primaries remain preserved components with the existing warning; 0.11.0 still owns arbitrary working-space conversion.

The native compositor remains RGBA8. Consequently, table values and processing intermediates stay floating point inside the LUT shader but the result is clamped and quantised when written to the adjustment output tile. RGBA64 documents continue through the authoritative 16-bit CPU compositor. This stage removes lookup-table quantisation; it does not claim a full floating-point document compositor.

`gpuHalfFloatCompatible` records whether finite table samples fit IEEE-754 binary16 and whether active domains can be represented as finite ordered f32 uniforms. Oversized or incompatible payloads are rejected before GPU preparation and retain CPU authority. A native allocation/pipeline failure is caught by startup parity and removes only the LUT adjustment feature bit. The diagnostic tests trilinear, tetrahedral, transfer-aware, Tony and AgX paths independently before publishing LUT acceleration.

The checked-in `shaders/adjustment_tile.wgsl` is the sole adjustment shader source. CMake reads and embeds it into a generated header consumed by `WebGpuContext.cpp`; the pre-build validator reads the same file. This eliminates the former checked-in-file versus embedded-string drift risk.


## 0.10.1e specialised LUT operator pipelines

`LutParameters::operatorProfile` separates ordinary table interpretation from named display transforms whose required preprocessing and postprocessing are not encoded in the `.cube` payload. The persisted values are `generic`, `tony-mc-mapface` and `agx-base-srgb`. Adjustment schema 10 stores the selection; schemas 1–9 migrate to `generic`, so reopening an older project never changes appearance because a historical source filename happens to resemble a named profile.

Fresh imports may suggest a profile from a normalised filename or `TITLE`, but that suggestion is made only by the parser at import time and remains manually editable in the Inspector. Both named profiles require a 3D table and force deterministic tetrahedral sampling.

The authoritative scalar order is:

1. interpret the document transfer as encoded sRGB, linear sRGB or unsupported preserved components;
2. convert recognised encoded sRGB values to linear Rec.709;
3. apply the named preprocessing transform;
4. apply any declared 1D shaper and then the 3D table with tetrahedral interpolation;
5. apply the named output operation;
6. return recognised linear output to the document transfer;
7. blend Strength against the original stored components;
8. clamp only at the RGBA8/RGBA64 destination write boundary.

Tony McMapface uses `x / (x + 1)` before its table and treats the table output as linear. AgX Base sRGB applies the supplied Rec.709-to-FilmLight-E-Gamut matrix, log2 allocation over `[-12.47393, 12.5260688117]`, the table, and a power-2.4 output operation. Unsupported ICC primaries receive no guessed gamut conversion and produce an Inspector warning; full arbitrary working-space conversion remains 0.11.0 work.

Generic and named pipelines share the validated RGBA16Float WGSL implementation on eligible 8-bit documents. The Inspector reports table/domain transport limitations, startup parity status and the deliberate exact CPU path used by 16-bit documents. Fallback remains local to the affected hierarchy and does not disable unrelated native features.

## 0.10.1d LUT processing contracts

`LutParameters::processingMode` makes the transfer-function contract surrounding the table explicit and persistent. **Encoded document values** means the LUT is authored for encoded sRGB-style values; **Linear sRGB / Rec.709** means it is authored for scene-linear sRGB/Rec.709 primaries; **Raw component values** means no transfer conversion is permitted. Adjustment schema 9 introduced this choice and schema 10 retains it alongside the operator profile. Schemas 1–8 migrate to Encoded document values because every earlier build sampled the document's stored components directly.

`CubeLut::evaluate()` receives the document transfer state separately from the persisted processing mode. For an sRGB document with a linear LUT, it decodes the input with the extended sRGB transfer function, evaluates the 1D/3D tables, re-encodes the output and then blends Strength in encoded document space. For a linear-sRGB document with an encoded LUT, the inverse conversion occurs around the table and Strength blends in linear document space. Raw mode bypasses both conversions. Negative and greater-than-one intermediate values are retained through the extended transfer functions and table stages; RGBA8/RGBA64 writes clamp explicitly at the destination boundary.

The current working-space model can identify sRGB and linear sRGB exactly. A valid different ICC profile is not silently converted through guessed primaries or transfer characteristics. The application preserves and samples its stored components and displays an Inspector warning for non-Raw LUT contracts. Full arbitrary input/working/output conversion is intentionally deferred to 0.11.0.

`CubeLut::requiresCpuEvaluation()` now reflects only payload/domain transport constraints; every persisted interpolation, processing and named-operator mode is implemented by the RGBA16Float shader. Runtime adapter/parity and deliberate 16-bit CPU decisions are reported separately. The tiled cache fingerprint includes interpolation, processing mode, operator profile, domains and table fingerprint.

## 0.10.1c authoritative CPU LUT evaluator

`CubeLut::evaluate()` is now the authoritative scalar LUT implementation. Evaluation is staged explicitly: the input is clipped independently to each 1D shaper domain, the optional shaper is sampled linearly, its output is clipped independently to each 3D domain, the red-fastest cube is sampled with the persisted interpolation mode, and Strength is blended against the original pre-LUT input. The scalar result is not clamped to display range. Existing integer image paths quantise and clamp only when writing RGBA8 or RGBA64 pixels, while a future floating-point compositor can retain the same extended values.

Both trilinear and tetrahedral interpolation are implemented. Tetrahedral selection uses a fixed branch order for exact fraction ties, making arithmetic order deterministic across builds. New 3D and combined `.cube` imports default to tetrahedral evaluation. Adjustment schema 8 introduced `interpolation`; schemas 9 and 10 retain it alongside the processing and operator contracts. Schemas 1–7 migrate to trilinear because that is the evaluator those projects were authored and previewed with. Domain-source metadata from schema 7 remains intact, while schemas 1–6 retain the existing legacy-domain migration.

This was the CPU-authority contract introduced in 0.10.1c. Since 0.10.1f, the native path uses RGBA16Float tables and implements the same trilinear/tetrahedral evaluator; 0.10.1g exposes that eligibility and any fallback reason in the Inspector.

## 0.10.1b LUT parser and data-model conformance

`CubeLut::parse()` is an explicit header-first state machine. Header directives are accepted only before the first numeric row; combined files consume exactly the declared 1D rows first and then the declared red-fastest 3D lattice. Duplicate title/size/domain/range declarations, generic `DOMAIN_*` mixed with LUT-specific input ranges, ranges for absent tables, unknown directives and recognised-but-unimplemented video-range flags are rejected rather than guessed. Errors identify the directive and source line wherever a line exists, while missing-row errors identify the declared and observed counts.

The parser no longer clips finite table entries or domains to ±16. Values must remain finite and representable by the existing float table payload; domains must have a finite positive span. `LutParameters` records whether each table domain came from the default range, `DOMAIN_MIN`/`DOMAIN_MAX`, a LUT-specific input range or legacy persistence. Adjustment schema 7 introduced that metadata; schemas 8–10 retain it. Public project format 14 and residency schema 15 remain unchanged.

## 0.10.1a LUT conformance baseline

The production `CubeLut` parser and evaluator remain unchanged in this first 0.10.1 stage. A separate `VFXPhotoLabLutConformanceTests` executable owns a hand-auditable fixture corpus, JSON vector manifest and independent scalar Cube parser/evaluator. The reference implementation keeps finite table values unmodified, evaluates combined 1D/3D files in declared order and computes red-fastest 3D addresses independently from production. Tests separate already-matching behaviour from deliberately pinned known defects, preventing a later parser, interpolation, colour-domain or GPU rewrite from silently redefining the expected result. See `LUT_CONFORMANCE_BASELINE_0.10.1a.md`.

The clipboard-to-document path reuses the existing exact private clipboard decoder rather than reading the lossy interoperability image when VFX Photo Lab owns the payload. `clipboardPayloadAsNewDocumentRaster()` validates the document-size ceiling, preserves source integer precision, QColorSpace/ICC and resolution metadata, converts scalar payloads to neutral straight RGBA, assigns sRGB only when no profile is present and normalises device-pixel ratio before `PhotoDocument::setSourceImage()` creates the base raster. Session creation remains detached until the complete raster is valid.

## 0.10.0l bounded document and layer decoding

Public project loading now performs a non-recursive raw-JSON preflight before constructing `LayerNode` or `VectorShape` containers. The preflight bounds the complete tree to 128 levels and 8,192 layers, verifies group child arrays, and counts vector objects and editable path nodes cumulatively across the project. Only a hierarchy that passes those limits reaches the normal schema-aware decoder. The same limits are enforced again on the decoded model before recursive ID, migration, normalisation or compositor work, so malformed external data and unsafe internal replacement transactions follow one contract.

`LayerNode::fromJson()` carries a shared remaining-layer budget through recursive group decoding, validates authoritative raster encodings, rejects non-finite opacity and legacy adjustment values, and decodes embedded PNG payloads through `QImageReader` with a 32,768-pixel extent ceiling. Optional damaged masks retain the established recoverable behaviour: the mask is discarded with one aggregated load warning rather than making the complete project inaccessible. Guide arrays are bounded to 65,536 entries per axis before sorting or allocation.

`VectorLayerData` now applies a one-million-node cumulative ceiling in addition to existing object/path limits. `PhotoDocument` applies a larger four-million-node aggregate ceiling across all vector layers, preventing many individually valid objects from forming an excessive project. These checks run on save, load, structural replacement and direct layer-tree replacement, which also covers Undo/Redo, Image/Canvas Size publication, Merge Layers and Hot/Warm/Cold restoration. No UI-only state or transient GPU surface is persisted, and all schema numbers remain unchanged.

The integration regression builds one RGBA64 document containing CPU Fill and Diamond Gradient output, a feathered selection, guides, a mask, opacity/blending, raster merge and semantic vector merge. It saves and reopens the project and compares the complete hidden-RGB-preserving render exactly, providing one cross-feature contract in addition to the existing focused tests.

## 0.10.0k.1 merged-vector active-object model

A vector layer may contain multiple `VectorShape` objects, but the path-editing UI previously treated `objects.first()` as the only editable payload. `MainWindow` now tracks both the active vector layer UUID and active object UUID. Object-aware hit testing walks vector objects in reverse draw order, and all path-node, Pen endpoint, contour, corner and keyboard operations resolve the active object by UUID before mutation. This keeps the merged layer as a true multi-object semantic layer while preserving independent object appearances and draw order.

Layer-level appearance controls deliberately iterate every object in the selected vector layer, because the user-facing merged layer owns one Inspector appearance edit. Geometry controls remain object-local. The two mutation scopes share the existing document preflight, revision, Undo and raster-cache invalidation paths; no active-object UI state is persisted in `.vfxphoto` or residency snapshots.

`LayerTreeWidget::selectionCommand()` returns `NoUpdate` only for right-button press/release events on an already selected model index. Qt still performs its normal focus/current-index and context-menu event handling, but the ExtendedSelection is not collapsed before MainWindow evaluates **Merge Selected Layers**.

## 0.10.0k Merge Layers

`LayerMergeOperations` separates compatibility analysis, detached result construction and tree replacement. A merge plan records the selected sibling IDs in top-to-bottom order, their common parent and the exact contiguous index range. The live document is not mutated while raster pixels are evaluated or vector paths are converted. Only after the complete result validates does `replacePlannedRange()` substitute one layer into a copied tree and `PhotoDocument::replaceLayerTree()` publish the transaction. MainWindow records the before/after states and layer selections as one Undo command.

Raster merging evaluates detached copies of the selected layers against a transparent bounded merge surface. The surface is derived from transformed raster-reference storage rather than the current canvas, so revealable off-canvas pixels are not discarded; unsafe projective domains or extents beyond the supported 32768-pixel storage limit fail before allocation. Each copy receives its authoritative world transform, so raster/mask reference origins, nested parent transforms, layer opacity and selected-stack blend modes are resolved exactly once. `renderPreservingHiddenRgb()` supplies visible Alpha and an independent RGB reference; storage is cropped to the bounds of any non-zero straight RGBA component, including meaningful colour beneath zero Alpha. The merged layer keeps the top layer ID/name, uses Normal/Copy at 100%, has no mask, and stores document-space pixels under the inverse parent transform so placement remains stable inside transformed groups.

Vector merging stays semantic. Preflight requires visible, unmasked, 100%-opacity Normal/Copy layers with affine transforms. Every object is converted to `VectorShapeType::Path`; rounded rectangles use the full world transform during conversion so document-pixel corner radii are baked from the visible outline. Object and layer transforms are then mapped into parent-local node/handle coordinates, layer order is reversed into the rasterizer’s bottom-to-top object order, compound contours/fill rules/strokes/dashes/arrowheads remain attached, and object/node UUIDs are regenerated. The result inherits only the common parent transform and therefore remains resolution-independent and directly editable.

## 0.10.0j.1 Gradient live-preview scheduling

Gradient placement keeps the full target source and selection coverage immutable, but no longer evaluates that source on the UI thread for each mouse event. `requestGradientPreview()` advances a generation and arms a 16 ms single-shot timer; the timer captures only the latest start/end geometry and launches one `QtConcurrent` worker. Pointer movement continues to update the fixed-screen placement overlay immediately. While a worker is active, later events set one pending flag rather than queueing work, and the next worker starts from the newest state after completion.

The worker evaluates `applyGradientCpu()` on a screen-resolution target tier derived from the current canvas viewport and capped to 960–1600 pixels on the longest side. It then replaces the target image in a detached layer snapshot and uses the existing interactive hierarchy compositor, selected-layer channel reference, or mask presentation path. Gesture serials prevent a worker from an older drag publishing into a newer drag; generation serials prevent an older completed frame from replacing a newer published frame; cancellation tokens terminate obsolete composite work during release, Escape, tool changes or session changes.

`ImageCanvas::setLiveCompositePreviewImage()` accepts the reduced transient frame and scales it only while painting the viewport. `commitLiveCompositePreview()` promotes same-size paint previews as before, but discards reduced Gradient frames instead of replacing the authoritative backing image. Release still evaluates the original full-resolution source and coverage through `RenderBackend::applyGradient()`, stores the same target-specific tile delta and schedules the normal authoritative preview. No document, clipboard, SVG, residency or persistence state contains the working-tier images.

## 0.10.0j Gradient Tool

Gradient is a press-drag-release raster edit built on the same authoritative `LayerEditTarget`, reference-size and transform resolution used by Fill and painting. `GradientCoverageRequest` maps the current sparse selection snapshot into raster- or mask-local coverage. The immutable source target and coverage are retained for the gesture; pointer movement changes only start/end geometry and a preview copy of the selected layer, so repeated live previews never accumulate or enter document history.

`gradientAmountAt()` is the shared geometry definition for Linear projection, Radial distance, wrapped Angle/conical phase, absolute Reflected projection and rotated Manhattan-distance Diamond gradients. `applyGradientCpu()` is the exact target reference for raster pixels, masks, greyscale and single RGBA components in 8- and 16-bit storage. Raster coverage interpolates in premultiplied space but writes straight RGBA; when output Alpha is zero the prior meaningful hidden RGB is retained. Scalar targets interpolate greyscale values, and Foreground-to-Transparent resolves to a zero endpoint because those targets have no independent colour Alpha.

Live preview maps document-space handles through the target inverse transform and the current preview scale, replaces only a detached preview-layer payload, then publishes through the existing composite/channel/mask presentation. The document remains unchanged until release. The authoritative result runs through `RenderBackend::applyGradient()` and `TiledCanvasEngine::applyGradient()`, after which raster, mask, greyscale or component changes are stored as one target-specific XOR tile-delta Undo command. Escape, tool changes and tiny click gestures discard preview state without history. Gradient participates in the document-mutation and interactive-publication guards so conflicting edits and document switches cannot overlap the gesture.

Gradient preferences live in `QSettings`, and neither live handles nor source snapshots enter `.vfxphoto`, clipboard, SVG or Hot/Warm/Cold data. Project format 14, vector schema 7, appearance schema 2 and residency schema 15 remain unchanged.

## 0.10.0i.1 Fill GPU shader hardening

The native Fill application shader no longer writes to a multi-component WGSL swizzle. It constructs complete vector values while preserving the existing deterministic CPU/GPU arithmetic. The shared shader-module helper rejects this known-invalid assignment form before wgpu-native can create or submit an invalid pipeline, including direct builds that bypass the supplied launcher scripts.

## 0.10.0i Fill Tool

Fill is a one-click raster edit that resolves the same authoritative `LayerEditTarget` used by painting and clipboard operations. The target can be raster pixels, a layer mask, the editable greyscale view or one RGBA component. Raster and mask reference extents/origins plus accumulated layer/group transforms map the document-space click and selection coverage into target-local pixels. Legacy deforming raster transforms are baked through the established safe rasterisation preflight before matching. Fill never silently creates a layer or writes into vector, text, adjustment or group payloads.

`buildFillCoverage()` is the deterministic region-selection boundary. It compares straight RGBA or scalar values against the clicked target pixel using a bounded 0–255 tolerance, performs either scanline connected flood fill or global matching, and multiplies accepted pixels by active selection coverage. Fully transparent raster pixels compare by Alpha only, preventing arbitrary hidden RGB from fragmenting visibly empty regions; hidden RGB remains untouched until the requested straight-RGBA edit is applied.

`applyFillCoverageCpu()` is the exact 8/16-bit reference. Partial raster coverage blends source and fill in premultiplied space while storing the result as straight RGBA; if output Alpha reaches zero, meaningful source RGB is retained. Preserve Transparency keeps Alpha fixed and interpolates RGB directly. Greyscale/channel components and 8-bit masks remain scalar coverage edits. `TiledCanvasEngine::applyFillCoverage()` partitions the bounded dirty region into 256×256 tiles and may dispatch the parity-approved RGBA8 WGSL application kernel; RGBA64 and every unsupported case use the same CPU reference. Any native tile failure discards the provisional GPU result and reruns the complete bounded operation through CPU, preventing mixed quantisation inside one Fill. The committed raster, mask, greyscale or component change is retained as one target-specific XOR tile-delta Undo command, including compact-mask before/after state. Project format 14, vector schema 7, appearance schema 2 and residency schema 15 are unchanged because Fill settings live in `QSettings`.


## 0.10.0h vector appearance preset storage

`VectorAppearancePresetStore` persists user styles outside the document model under the application-data `presets/vector-appearance` directory. Each file uses a version-1 `vfxphotolab-preset` envelope with category `vector-appearance`; the nested payload is the existing schema-2 `VFXPhotoLabVectorAppearance` object, so preset coverage remains exactly aligned with Copy/Paste Appearance and future appearance-schema migrations. Writes use `QSaveFile`, names are bounded and collision-resistant on disk, and loading limits count and byte size while rejecting malformed, duplicate or wrong-category files.

The preset manager obtains its source from the first selected vector object, falling back to the active Shape/Pen defaults. Application reuses `applyVectorAppearanceToSelection()`, which preflights detached copies of every selected vector layer and commits one document-state Undo entry. Applying to tool defaults reuses `syncVectorAppearanceDefaults()` and `QSettings`. Presets are not embedded in `.vfxphoto`, clipboard, SVG or Hot/Warm/Cold snapshots; project format 14, vector schema 7, appearance schema 2 and residency schema 15 therefore remain unchanged.


## 0.10.0g.2 local endpoint-cap trimming

Arrowhead marker geometry is still derived from the transformed open centreline and document-pixel stroke width. The 0.10.0g.1 implementation removed cap protrusion by intersecting the complete shaft outline with an endpoint-aligned inward half-plane. That was geometrically correct only for simple paths: a winding path could place unrelated earlier segments in the outward half-plane and lose them wholesale.

`clipStrokeCapAtEndpoint()` now returns Butt-cap shafts unchanged and, for Round or Square caps, transforms the outline into the endpoint's orthonormal frame and subtracts only the bounded half-stroke cap footprint immediately outside that endpoint. The operation cannot reach a distant segment merely because it lies in front of the same endpoint plane. Marker union, conservative bounds, requested-region rasterisation and Expand Stroke continue to consume the same corrected `strokePathForWorldTransform()` result.

## 0.10.0g.1 vector arrow architecture

`VectorStroke` stores independent start/end `VectorArrowheadType` values and scale multipliers. Markers are presentation geometry derived from the transformed open centreline and document-pixel stroke width, so object and nested-layer transforms change tangent orientation and endpoint placement without destructively scaling stroke appearance. Each marker is centred longitudinally on its endpoint: a pointed head extends beyond the centreline while its base remains behind it, and a circle is centred directly on it. For an endpoint carrying a marker, the generated shaft outline has only the bounded Round/Square cap footprint immediately outside the endpoint removed before marker union, preserving winding-path segments elsewhere while still preventing cap protrusion at the minimum marker scale. `strokePathForWorldTransform()` then unions marker coverage with the clipped shaft; bounds, tile culling, rasterisation and Expand Stroke therefore share one source of truth. SVG marker definitions use the same centred reference frame.

The semantic `VectorShapeType::Arrow` is separate from stroke markers. It retains a normal shape bounds rectangle plus head-length and shaft-width ratios, generates a closed seven-vertex block-arrow path, and converts to ordinary Bézier nodes through the existing Convert Shape to Path operation.

Public project format 14 loads versions 1–13 and rejects relabelled older projects containing arrow semantics. Vector schema 7 migrates schemas 1–6, appearance schema 2 migrates schema 1 with None/1× marker defaults, and private residency schema 15 reads snapshots 2–14.


`LayerNode` remains the authoritative recursive document model. Editable paths extend `VectorShape` through vector payload schema 7. The primary contour remains `bezierPath` for compatibility with the established Pen, Direct Selection and Corner workflows; `additionalBezierPaths` stores zero or more independent closed contours for compound fills. Every contour retains ordinary stable node UUIDs, anchors, handles, node modes and live-corner metadata. `pathFillRule` persists either even-odd or nonzero winding and is applied only when generating the transient `QPainterPath`; Bézier nodes remain the source of truth.

Public project format 14 adds arrowhead and semantic Arrow metadata while loading versions 1–13. The existing downgrade guards still reject pre-version-9 paths, pre-version-10 live corners, pre-version-11 dashed strokes, pre-version-12 compound contours and pre-version-13 nonzero-winding paths when those semantics are dishonestly relabelled. Private residency snapshots use schema 15. Vector schemas 1–6 continue to load and migrate to schema 7.

The Pen and Shape toolbars share one persisted vector-appearance default set: secondary-colour fill, primary-colour stroke, enabled flags, width, pattern, dash/gap/offset, cap, join, alignment, miter limit and independent start/end arrowheads with scales. New open Pen paths retain the fill preference but are kept visibly stroked while unfinished; closing from the Pen Tool hides endpoint markers nondestructively and applies the current closed-path Fill/Stroke toggles and alignment in the same undoable close operation.

## Explicit semantic-shape conversion

`VectorShape::convertToPath()` is the single primitive-to-Bézier conversion boundary. Rectangle, Ellipse, Polygon, Star and Line convert from their existing deterministic semantic `QPainterPath`; appearance, object UUID, object transform and layer transform remain untouched. The generated `VectorBezierPath` owns new unique node UUIDs and becomes the persisted geometry source immediately.

Rounded Rectangle conversion is appearance-aware. Its radii are defined in document pixels and `pathForWorldTransform()` may intentionally differ from a naively transformed local rounded rectangle. Conversion therefore captures that exact visible path in document space, maps it back through the invertible layer/parent and object transforms, and stores the resulting local Bézier geometry. Reapplying the unchanged transforms reproduces the pre-conversion outline without stretching the visible corners.

The MainWindow command preflights every eligible object in all selected vector layers on detached layer copies, validates the resulting current schema-7 vector payloads, then commits them together and records one document-state Undo command. A failed conversion or replacement restores the untouched before-state; no partial layer set is published. That primitive conversion itself introduces no compound metadata; project, clipboard, SVG, Image Size and Hot/Warm/Cold paths continue to use the same ordinary Bézier representation.

## Expand Stroke

`VectorShape::expandedStrokePath()` asks the existing stroke resolver for the exact visible document-space path, so width, alignment, caps, joins, miter limits, dashes and object/layer/group transforms use the same geometry as settled rendering. The resolved outline is mapped through the inverse containing-layer world transform. Stroke colour/opacity become fill colour/opacity; the output has no residual stroke or object transform.

The contour extractor retains each closed `QPainterPath` subpath independently. Ordinary dashed strokes therefore become compound paths containing one contour per dash island, while closed solid strokes retain separate outer and inner contours for real negative space. `VectorShape::geometryPath()` applies the persisted even-odd or nonzero fill rule; generated stroke outlines use nonzero winding so touching dash islands remain a union. No zero-area hub bridges, connector spokes or seam-producing retraced segments are persisted. If exact contours do not reproduce source coverage, Qt simplification is attempted. Geometry that still cannot be represented safely is rejected and the original document remains untouched.

Direct Selection and Corner Tool display nodes only for the active contour, avoiding the dense overlay and hit-testing cost of every dash at once. Clicking another contour boundary swaps it into the primary editable slot without changing fill coverage, object identity or visible ordering and without creating Undo history. Deleting every node in that active contour removes only the contour when another remains. UUID normalisation, cache fingerprinting, memory estimates, Image Size scaling, duplication, clipboard JSON, project save/reopen, residency and SVG all traverse every contour. Standard SVG export emits one `Z`-closed subpath per contour with the persisted `fill-rule`; import restores both even-odd and nonzero compound fills. Exact VFX metadata remains authoritative for round-tripping node identities and semantic appearance.

At the document layer, a stroke-only single-object vector layer is replaced directly by its outline. When a visible fill or multiple imported SVG objects must remain, the original layer becomes an Isolated group with one vector child per render component. Children are stored in reverse layer-stack order so compositing reproduces the original per-object fill-then-stroke sequence. The parent retains the original transform, mask, opacity, blend mode and visibility, ensuring those properties are applied once to the same combined image. The entire selected-layer set is preflighted and committed under one structural Undo state; any failure restores the complete before-state.

## Pen continuation and path joining

Pen continuation is transient editor state, not persisted geometry metadata. `m_penAppendAtStart` records which endpoint emits the next segment, allowing start-side continuation to prepend anchors without reversing the stored path. This preserves node identity/order, dash direction and the later start/end-arrowhead meaning. The opposite endpoint is the only closure target while a path is active; both endpoints remain ordinary continuation targets when no Pen session is active.

Open-path joining is performed in the active path's local coordinate system. Donor anchors and both handles are mapped through the donor object/layer/group transforms into document space, then through the inverse active layer/object transforms. The donor is oriented so the clicked endpoint meets the active endpoint. Separated endpoints retain a normal connecting segment; coincident endpoints merge into one junction that preserves the active incoming side and donor outgoing side as independent Corner handles. The active object's appearance and identity remain authoritative. The consumed donor object is removed in the same atomic document-state command; its layer is removed only when that object was the layer's sole vector object, preserving sibling objects from imported SVG content. Any replacement/removal failure restores the complete before-state.

Endpoint feedback is presentation-only. `MainWindow` gathers visible open-path endpoints in document space and classifies them as Continue, Close, Join or Active. `ImageCanvas` draws fixed-screen markers and hover emphasis without changing path data, render generations or project history. Hit testing prioritises the active Close endpoint over overlapping external Join targets. Escape restores the gesture's captured before-state only while an anchor press/drag is live; otherwise it finishes the transient Pen session without altering document history.

The Pen tool creates straight anchors by clicking and symmetric handles by dragging. The Direct Selection tool edits anchors and handles using fixed screen-pixel hit targets. **Make Sharp** collapses both handles into the anchor and clears live-corner state, while **Make Corner** preserves the handles but decouples their movement. Selection, mode conversion, node insertion, deletion and path opening/closing remain atomic document operations. Path coordinates stay local to the vector object; object and layer transforms remain nondestructive and are reused by groups, masks, multi-layer transforms and off-canvas storage.

Direct Selection and Corner marquee selection is presentation-only state owned by `MainWindow`/`ImageCanvas`; it changes the selected node-index set without touching document history. Enclosed anchors are tested in document space after the complete object/layer/group transform. Shift-constrained handle movement quantises the pointer direction against absolute document axes in 45-degree increments before transforming it back into path-local coordinates.

Path geometry uses deterministic CPU `QPainterPath` construction. Requested vector tiles are rasterised through the existing bounded `VectorRasterizer`, then enter the native tiled WebGPU hierarchy compositor where eligible. RGBA64 documents remain on the exact CPU reference path, preserving straight Alpha and hidden RGB beneath zero Alpha.

## Semantic stroke patterns

`VectorStroke` stores Solid/Dashed pattern state, dash length, gap length and dash offset in document pixels alongside width, alignment, cap, join and miter limit. Geometry generation converts those pixel values to Qt's width-relative custom-dash units only at the `QPainterPathStroker` boundary, so resizing and serialization remain independent of renderer conventions. Cap choice is applied by the stroker to each dash and to open path ends.

Shape-tool defaults bind fill to the shared secondary colour and stroke to the shared primary colour. The top-bar popup and vector Inspector mutate the same semantic fields; they do not bake or rasterise the outline. SVG standard geometry maps the pair to `stroke-dasharray`, `stroke-dashoffset` and `stroke-linecap`, while exact Photo Lab metadata preserves schema-4 values.

## Live vector corners

A live corner remains attached to its original sharp path node as `cornerRadius` plus `cornerStyle`; it does not rewrite neighbouring anchors on every adjustment. Eligible nodes are closed-path Corner nodes whose adjacent segments are straight. Generated Rounded, Chamfer, Concave and Cutout outlines are deterministic `QPainterPath` geometry used by rendering, hit testing, bounds, thumbnails and ordinary SVG export.

The Corner Tool stores one immutable path at gesture start and changes only selected node corner fields during pointer movement. Semantic primitive layers convert to paths only when targeted. **Bake Corners** converts the generated outline back into ordinary line/cubic nodes, removing all live-corner state and returning full control to Direct Selection. Exact `data-vfx` SVG metadata retains live values; external SVG consumers receive the already generated visible path.

## Interactive vector performance

Pointer-rate node and handle edits use a dedicated known-changed document mutation path. The gesture captures one complete immutable before-state, detaches the semantic path once, then mutates only affected nodes; it does not copy, normalise and deeply compare every anchor on each mouse event. Authoritative normalisation and one atomic Undo command still occur on release.

Path snapping targets are collected once at gesture start and reused until release or path selection changes. Interactive preview requests are coalesced into one visible level-0 region with ordered publication, allowing the current complete frame to finish instead of cancelling every GPU readback and starving presentation. Settled rendering returns to exact-generation tiled publication.

`VectorRasterizer` maintains a globally bounded transformed-geometry cache keyed by layer/object revisions, semantic fingerprints and world transforms. Base paths and stroke outlines are shared across requested tiles, and bounds are combined without expensive boolean path unions. Pure vector/text translation reuses the existing sharp transform foreground and is applied directly by the canvas; scale, rotation and text-box changes retain semantic rerasterisation so document-pixel strokes and rounded corners remain correct.


## SVG interoperability

`SvgWorkflow` is a bounded semantic interchange layer, not a second renderer. It parses practical SVG XML directly with `QXmlStreamReader`, maps supported geometry into existing `LayerNode`, `VectorLayerData`, `VectorBezierPath` and `TextLayerData` structures, and leaves the established tiled rasteriser/compositor unchanged. DTD/entity input is rejected and file, element, nesting, path-node and coordinate limits are enforced before imported layers enter the document.

External SVG path commands are normalised to the editor's cubic-node model. Quadratic controls are converted analytically and elliptical arcs are split into at most quarter-turn cubic segments. Unsupported compound-path semantics are surfaced because the current vector object represents one editable subpath rather than a compound winding tree.

Export writes normal SVG geometry first. Compact `data-vfx` JSON metadata on vector and text layer groups preserves Photo Lab-specific semantic details that standard SVG cannot represent exactly, such as independent circular corner radii, regular star parameters, area-text overflow and inside/outside document-pixel stroke alignment. Reimport prefers validated metadata and otherwise uses the ordinary SVG fallback, so files remain interoperable rather than proprietary containers disguised as SVG.

## 0.9.0f integration and trust boundaries

SVG limits are cumulative as well as local. Import tracks accepted editable leaves, vector objects, Bézier nodes and text characters, then validates the complete group tree before it can enter `PhotoDocument`. Export performs an equivalent preflight before opening its atomic `QSaveFile`, so malformed in-memory callers cannot provoke deep recursion, singular transforms or oversized semantic payload generation.

SVG container `display` is represented by the container layer's visibility, while inherited SVG `visibility` is resolved on descendant leaves. This distinction allows a child to restore `visibility:visible` inside a hidden SVG group without weakening `display:none`. Definitions such as `<symbol>` are not treated as painted content in the absence of editable `<use>` support.

The File-menu SVG actions now share the same semantic-content predicate used by export: vector and text leaves are exportable, groups are exportable only through such descendants, and raster/adjustment-only selections do not open a save dialog. Active canvas text editing is committed before SVG import/open/export crosses a document mutation boundary.

## Keyboard node editing

Direct Selection and Corner Tool keyboard commands are routed only while the canvas owns focus. Ctrl+A and Escape mutate editor selection state only; they never create structural history entries. Arrow movement conjugates the requested 1 px or 10 px document-space translation through the complete object/layer/group transform before applying it to every selected `VectorPathNode`, preserving exact document-axis movement even for projective transforms. `VectorBezierPath::transformNodes()` stages the complete path copy, validates all moved anchors and handles, and publishes only if the resulting path remains safe, preventing partial movement near coordinate limits.

A keyboard nudge records one ordinary structural Undo command containing the full before/after document state and selected layer set. Delete uses the same atomic boundary. When every node is selected, only the first active path object is removed from a multi-object vector layer; the containing layer is removed only if that path was its sole object. This preserves imported SVG siblings and keeps clipboard/project/residency semantics unchanged.

## Vector appearance quick actions

`VectorAppearance` is a geometry-free value object containing the complete currently supported `VectorFill` and `VectorStroke` state. Its schema-1 JSON form is bounded to 64 KiB on clipboard import and requires the explicit `VFXPhotoLabVectorAppearance` format marker before it can enter the document. Geometry, object IDs, transforms, layer opacity and project state are intentionally excluded.

Swap exchanges the fill/stroke enabled flags, colours and opacities while leaving stroke-only geometry fields attached to the stroke. Reset chooses fill-only defaults for closed shapes and centre-stroke defaults for open paths and lines. Applying an appearance always passes through `VectorShape::normalise()`, so line/open-path invariants remain valid. Multi-layer application prepares and validates every updated `VectorLayerData` before replacing document layers and records one document-state Undo command.

When no vector layer is selected, Shape and Pen tools can receive pasted, swapped or reset appearance defaults. These defaults persist fill/stroke enabled state, both opacities, colours, width, dash pattern and offset, cap, join, alignment and miter limit through `QSettings`; no project schema is involved.

Canvas panning has higher pointer priority than vector-path hover and editing once a pan gesture begins. Middle-button and Space+left panning therefore remain available in Pen, Direct Selection and Corner Tool without publishing accidental vector pointer moves.
## Vector hover and component targeting

Hover state is presentation-only MainWindow/ImageCanvas data. `PathHit` identifies an anchor, incoming handle, outgoing handle, live-corner control or Bézier segment in viewport-pixel distance; it never enters `VectorLayerData`, project persistence, clipboard payloads, history or residency snapshots. `CanvasVectorHover` carries only the current visual target and an optional document-space segment path to the canvas overlay painter.

Point controls are resolved before segments. Within the visible point controls, a small weighted screen-distance bias gives selected-node handles and live-corner controls precedence at true overlaps without allowing a distant handle to steal a clearly closer anchor. The same hit result drives both hover painting and the following press, preventing visual feedback from disagreeing with the component that is dragged.

Overlay geometry is mapped to viewport coordinates before painting. Anchor boxes, handle circles, endpoint markers, outlines and hit tolerances therefore remain fixed in screen pixels across zoom levels. Hover is cleared on canvas leave, path/layer invalidation, tool changes and pan start; it does not schedule semantic rerasterisation or create Undo history.


## 0.10.0f.2 dashed contour acceptance

The platform path stroker may publish zero-area move/closure fragments next to the actual filled islands of a closed dashed stroke. Contour extraction now measures each isolated subpath and discards only fragments with no meaningful fill area before converting the remaining contours to `VectorBezierPath`. Every visible island is still validated, bounded and included in the final odd-even compound path. Polygon area is measured after translating to a local origin to preserve precision for tiny dashes at large document coordinates.

## 0.10.0f.3 path fill-rule correction

`QPainterPathStroker` emits stroke coverage using nonzero winding. Earlier compound-path hardening persisted every multi-contour result as even-odd. That is correct for a ring when contours are treated purely by parity, but it is wrong for dashed islands that touch or overlap at a corner or closed seam: parity cancels the overlap. Expand Stroke now marks generated outlines as `VectorPathFillRule::NonZero`, preserves the original contour directions, and validates against the same winding rule used by the source stroke. Existing authored Photo Lab paths default to even-odd for backward compatibility. SVG `fill-rule="evenodd"` and `fill-rule="nonzero"` map directly to the same persisted field.

## 0.11.0e OCIO boundary

`OcioIntegration` is an optional backend layered beside the existing ICC/Qt transform service. A document stores an `OcioConfigReference` with a stable source, identifier, resolved fingerprint, version and ICC interchange space. `ColourSpaceDescriptor::Ocio` always carries the same config identity and fingerprint; colour-state validation rejects cross-config descriptors.

Built-in ACES 2 configs, `$OCIO`, and external config files are inspected before use. External/environment configs are reloaded for inspection so file changes can be detected. Built-in configs may be cached because their versioned URI is immutable. A saved fingerprint mismatch blocks conversion until the user explicitly relinks.

ICC-to-OCIO and OCIO-to-ICC conversion uses the OCIO built-in `sRGB - Texture` interchange plus the existing cached Qt ICC transform where required. Direct OCIO-to-OCIO conversion uses the selected config processor. Alpha is copied unchanged and hidden RGB is transformed even when Alpha is zero. Integer storage clamps to the document's 0–1 range; unbounded scene-linear storage is deferred.

Display/view selections introduced in 0.11.0e remain configuration metadata. In 0.11.0f they are consumed only by the presentation pipeline described below; ordinary OCIO colour-space conversion continues to use `OcioCpuTransform`.

## 0.11.0f presentation boundary

`DisplayColourTransform` is an immutable CPU reference object built from a `DocumentColourState` and the active `MonitorProfileInfo`. Its fingerprint includes the complete semantic colour state and monitor-profile identity, so monitor, display/view, proof or compatibility changes invalidate only derived presentation copies.

The ordered stage chain is:

1. Interpret the authoritative preview in the document working space. Untagged working data uses an explicit presentation-only sRGB assumption and reports a warning.
2. When enabled, convert to the selected proof ICC profile. Rendering intent and black-point-compensation are retained in the transform request; the Qt ICC engine applies the capabilities it supports.
3. Convert either to the detected/manual ICC monitor profile or through a distinct OCIO `DisplayViewTransform`. ICC documents bridge to OCIO through the built-in sRGB texture interchange space. Configuration fingerprints must match before a processor is created.
4. Untag the already display-encoded presentation image before `QPainter` draws it, preventing a second implicit conversion.

`ImageCanvas` retains the original working-space `m_image`, progressive tile images, live-stroke image and transform-preview images. Parallel `m_display*` surfaces are rebuilt from those originals and are the only surfaces selected by `paintEvent`. Sampling, paint commits, transforms, selections, Copy Merged, Merge Layers, save and export continue to read the original surfaces. Document-strip thumbnails follow the same rule: residency keeps a raw working-space thumbnail, while the strip receives a short-lived display-transformed copy.

Monitor discovery is deliberately best-effort and non-blocking after the first lookup for a screen identity. Linux queries `colormgr` for the display device and its default profile; Windows queries Windows Color System. Manual and environment overrides take priority. Invalid or unavailable profiles produce one visible status and an sRGB fallback rather than repeated modal warnings.

Colour-state schema 4 adds `presentationColourManagementEnabled`. Newly managed documents enable automatic monitor ICC presentation. Schema-1/2/3 projects migrate with the gate disabled, so merely opening an older project cannot change its canvas appearance. Display/proof metadata persists through public projects and private Hot/Warm/Cold snapshots, but `MainWindow::applyDocumentState` keeps live presentation choices outside ordinary Undo/Redo.

## 0.11.0g GPU colour-transform evaluation

`DisplayColourTransform` still owns the authoritative CPU stage list. On first validated GPU use it lazily bakes the complete stage chain into a 65³ RGBA16Float lattice; a proof round-trip lattice is added only for gamut warning. The lattice fingerprint derives from the complete display-transform fingerprint and warning mode.

`RenderBackend` admits the GPU path only after native WebGPU foundation approval and the display-specific parity diagnostic. Each complete lattice is checked against deterministic exact CPU probes before admission; transforms above the approximation limit remain CPU-only. A bounded shared CPU-lattice cache avoids duplicate baking across short-lived transform objects and documents. `WebGpuContext` caches uploaded lattices under a bounded LRU budget and evaluates them with manual trilinear WGSL sampling for RGBA8 or RGBA16Float presentation surfaces. RGBA16 Alpha is restored exactly after readback. Any lattice, shader, adapter, storage-format, cancellation or parity failure returns an empty GPU result and `ImageCanvas` immediately executes the original CPU transform.

The QPainter presentation boundary is unchanged: transformed GPU results are read back into derived `QImage` surfaces. Raw document and residency caches remain monitor-independent, and export has no access to this path.

Managed adjustment-domain acceleration uses the same reference-baking principle but remains inside the tiled compositor. `ManagedAdjustmentGpuLutData` stores paired 65³ RGBA16Float working→domain and domain→working lattices keyed by the complete working-space fingerprint and processing domain. `TiledCanvasEngine` attaches a pair only for `ManagedV1` adjustments whose declared contract is Linear Working or Encoded sRGB/Rec.709. The WGSL pass quantises after the forward transform and after the operator, applies the inverse transform, then performs opacity/mask/blend operations in the document working space.

The ordinary per-adjustment WGSL parity mask remains authoritative for operator math. A second startup gate validates the complete managed path with Display-P3 linear-working Exposure and encoded-sRGB Saturation. Every actual profile pair also passes deterministic CPU probes before upload. Separate bounded CPU and GPU LRU caches reuse identical pairs across tiles, documents and frames. If the global managed-domain gate or an individual pair fails, only that managed hierarchy returns to `ImageProcessor`; unrelated approved adjustments and presentation transforms remain accelerated.
## 0.11.0h colour-managed export boundary

Flattened-image export snapshots the document source, layer tree and `DocumentColourState`, then renders through `ImageProcessor::renderPreservingHiddenRgb`. The result is authoritative working-space data and never a presentation copy. `ImageExport` promotes it to straight RGBA64, applies an ordinary `WorkingToOutput` ICC/OCIO conversion, optionally composites an output-space matte, then encodes 16-bit or reduces to 8-bit.

`ImageExportCapabilities` is resolved from the destination suffix before work begins. PNG/TIFF expose 16-bit integer output; all supported formats expose 8-bit. Alpha, ICC metadata and quality controls are enabled only where the export implementation declares them. Formats without Alpha flatten after colour conversion. Formats without reliable ICC writing strip `QColorSpace` metadata and return a user-facing warning.

The deterministic 64 × 64 blue-noise rank tile is checked into `BlueNoise64.h`. Quantisation applies one centred threshold to RGB, preserves exact endpoints and quantises Alpha independently without noise. The seed and image dimensions choose a stable tile phase.

`PhotoDocument::replaceOutputColourSettings` changes only `DocumentColourState::output`, keeps the processing revision stable and does not invalidate compositor, histogram or presentation caches. The complete colour state already travels through project and private Hot/Warm/Cold serialization.

The export worker uses the existing cancellation token and writes through atomic `QSaveFile` paths. Monitor ICC, OCIO Display/View, proof and gamut-warning settings are intentionally absent from the request and transform creation.

## 0.11.0i persistence and resource-audit boundary

`ColourResourceAudit` inspects the external dependencies named by a saved `DocumentColourState` without mutating that state. External ICC descriptors keep their embedded ICC bytes and SHA-256 fingerprint as the authoritative reproducible profile. The original path is treated as a relink source: deletion, unreadability, excessive size, invalid contents or a changed fingerprint produces a warning when the embedded copy is valid, and a blocking issue only when no reproducible copy exists.

OpenColorIO references remain identity-bound to their saved source and fingerprint. Audit resolution may inspect the referenced configuration, colour spaces, display, view and look, but it never chooses a different configuration or semantic option. Missing/fingerprint-mismatched runtime dependencies therefore disable only the transform that requires them and preserve the saved state for explicit relinking.

`PhotoDocument::loadWarnings()` remains reserved for actual persisted-data repair. `colourResourceWarnings()` is a transient load/restore diagnostic and is not serialized. `MainWindow` marks a project modified only for real repairs; colour-resource warnings are consolidated into a one-time open report. `SessionSnapshotCodec` re-audits after Cold restoration so filesystem changes that occurred while a document was evicted are visible without polluting the private snapshot format.

Presentation and output metadata setters are idempotent. Reapplying semantically equal settings leaves the modified flag, processing revision, source/layer cache keys and Undo stack unchanged. Actual metadata edits remain save-worthy but still do not enter pixel-processing identity or Undo history.

The final hardening stage intentionally keeps project format 15, snapshot schema 16 and colour-state schema 4. It strengthens validation (including exact SHA-256 external fingerprints and strict source-image base64) rather than creating a migration that could alter existing documents.


## 0.12.0c export profiles

`ExportProfileData` and `ExportNamingTemplate` live in `vfxphotolab_core`; the Widgets-only `ImageExportDialog` consumes them. Export profiles are application-global preset data and do not become document/session state. The dialog materialises one `ImageExportRequest`, then the unchanged full-resolution render/colour-conversion/quantisation/atomic-write pipeline executes it.

A naming template resolves only a portable stem under the directory selected by `QFileDialog`. It cannot alter the parent directory. The resolved extension comes from the profile format. 0.12.0d composes several independently validated profile-derived outputs without changing this single-output contract; 0.12.0e adds in-session queue ownership and 0.12.0f adds recoverable descriptions/restoration.


## 0.12.0d production multi-output boundary

`ProductionExportPlan` is an immutable-by-copy job description assembled by `ProductionExportDialog`. Each enabled `ProductionExportOutput` has a stable job-local ID, a stable library profile ID/name, a complete `ExportProfileData` snapshot, an independently editable naming template and an explicit resize contract. The snapshot prevents a profile rename, update or deletion from silently changing an already configured output.

`resolveProductionExportPlan()` is the deterministic preflight boundary. It rejects missing/duplicate output IDs, invalid profile or resize payloads, unsafe surface sizes, unsupported writers, colour incompatibility, invalid filename templates and duplicate destination paths before rendering begins. It returns a fresh vector only on complete success, so callers never observe partially resolved plans.

The execution path renders the document once with `ImageProcessor::renderPreservingHiddenRgb()`. Each output then obtains its own working-space image surface: original size reuses the immutable render, nearest/bilinear prefer `RenderBackend::resampleImageTiled()`, and all methods can use `resampleStraightRgbaCpuReference()`. Colour conversion, bit-depth encoding, dithering, Alpha/matte handling and ICC embedding remain exclusively in `prepareImageExport()`; the production layer does not create a second colour pipeline.

Each prepared output is immediately passed to `writePreparedImageExport()`, which retains the existing atomic `QSaveFile`/TGA publication boundary. Prepared images are not accumulated across the job. Resize, transform or writer failure is recorded per output and execution continues, while cancellation stops future outputs and cooperatively interrupts active render/resample/colour work.

Collision policies are explicit job data: ask-before-replace, overwrite, skip-existing or auto-rename. Preflight resolves all intra-job collisions; execution rechecks newly appeared files so skip and auto-rename remain safe, and ask-before-replace does not overwrite a file that appeared after confirmation.

0.12.0f persists unfinished queue descriptions only in versioned private application-data recovery files. Jobs are restored as non-writing Recovered records and require explicit revalidation/resume. Queue data remains outside `.vfxphoto`, Hot/Warm/Cold state, Undo history and presets. Folder-wide discovery and automation remain 0.19.0.

## 0.12.0e application export queue

`ExportQueueController` owns a bounded vector of job records and exactly one `QFutureWatcher` at a time. Each record combines lightweight `ExportQueueJobInfo` with a captured `ExportQueueEnqueueRequest`. The request contains copy-on-write source/layer data, the colour-processing contract and the already resolved 0.12.0d output plan. No record points at a live document session.

`ExportQueueCore` defines stable states, terminal/cancellation/removal rules, legal transitions, progress normalization and portable UUID validation. `ExportQueueDock` is a modeless view/controller surface; it never executes image work directly.

The controller's shared `Control` object contains atomic cancel/pause flags plus a condition variable. Cancellation is passed into the established render, resize and export preparation calls. Pause waits only at safe checkpoints, avoiding suspension while native resources or atomic writers are in an indeterminate state.

Production rendering still allocates one full-resolution composite and at most one derived output surface. The renderer's existing mutexes serialize shared GPU/native work with interactive rendering. Terminal jobs release source/layer/output snapshots immediately while retaining counts and text details for the dock.

Normal application close refuses to tear down a running controller. Confirmed cancellation keeps `MainWindow` alive until the controller emits its idle boundary; the destructor also drains the controller before ordinary QObject child teardown. Cross-process recovery is not part of this stage.


## 0.12.0f recovery boundary

`ExportQueuePersistence` owns versioned atomic recovery files. The controller writes a recoverable description before accepting a job, removes it on terminal completion/cancellation, and restores valid files as `Recovered`. The worker selects only `Pending`, so restart never writes automatically. Explicit resume reconstructs resolved outputs from the saved executable plan and current filesystem. Disabled production-dialog drafts are omitted because they were never part of the resolved job.

The raw source snapshot uses compressed straight RGBA8/RGBA16 rows to preserve component depth and hidden RGB. Layer trees reuse the bounded versioned layer JSON contract; any compatibility warning during recovery is treated as rejection rather than silent repair. Preservation also advances a per-job execution generation, so progress callbacks already queued by a cancelled worker cannot mutate a Recovered record or a later replay.


## 0.12.0g integrated hardening boundary

`validateResolvedProductionExportOutputs()` is the trust boundary between dialog preflight and durable queue ownership. It proves that every resolved row is a one-to-one representation of an enabled `ProductionExportOutput`: stable IDs and profile references match, resize dimensions/method and safe surface limits match, the filename resolves under the selected directory, auto-renames are canonical siblings, output paths are unique, encoding/colour/Alpha/dither settings equal the captured profile, and the required writer remains available. It deliberately does not recalculate collision outcomes; execution and recovered-job resume retain their separate live-filesystem checks.

`ExportQueueController::enqueue()` performs all deep snapshot/colour/plan validation and commits the private recovery record before it mutates bounded queue history. A rejected enqueue therefore cannot remove an older terminal record. The queue UUID is checked against both in-memory jobs and existing recovery files before acceptance. Source snapshots must already be straight RGBA8 or RGBA16 so in-session and recovered rendering consume identical component semantics.

Skip metadata is an observation from dialog preflight, not an execution command. Every configured output reaches the worker collision check. If the colliding file disappeared while the job waited, the output is prepared and written; if it still exists, Skip Existing records a deliberate skip. Progress maximum includes render plus every configured output, so completed, skipped and failed rows all consume one deterministic unit.

Preserve-on-quit waits for the worker and classifies the returned result before converting unfinished records to Recovered. A result is complete only when it has exactly one successful-or-skipped result for every expected output, no render failure and no cancellation. Such a job is finalised immediately and its private recovery file is removed; partial or retryable work remains recoverable. Execution generations continue to reject stale queued callbacks.

Recovery layer validation is iterative and bounded by both count and `LayerNode::MaximumTreeDepth`, preventing malformed hierarchy recursion from exhausting the process stack. Recovery writes also preflight available storage and use `QSaveFile` with direct-write fallback explicitly disabled. The same explicit no-fallback atomic contract is applied to raster/TGA export and unified preset writes.

No 0.12.0g data enters `.vfxphoto`, Hot/Warm/Cold snapshots, Undo history or colour-state JSON. Public project format 15, private snapshot schema 16, colour-state schema 4, adjustment schema 10 and vector schema 7 remain unchanged.

## 0.13.0a spatial-filter boundary

`SpatialFilterContract` is an in-memory processing contract, not persisted document state. It describes a filter footprint in document pixels, independent X/Y preview scaling, edge mode, Alpha semantics, preview quality, bounded padding and a deterministic fingerprint. `SpatialFilterTilePlan` converts that contract into one output region, an explicit sampling rectangle, in-bounds dependency regions, conservative dependency bounds and a crop offset.

The current compositor can consume only one contiguous dependency rectangle, so wrapped dependencies at opposite edges may conservatively unite to the full source width/height. The contract retains the disjoint region list for later Live Filter caches. Mirror also chooses a conservative full-extent dependency when a footprint crosses an edge. Clamp and Transparent use the clipped expanded region. The inverse `affectedOutputRegions()` mapping supplies the corresponding dirty-region hook.

The 64-byte `SpatialFilterGpuContract` has compile-time size, alignment and field-offset assertions and is mirrored by `shaders/spatial_filter_fixture.wgsl`. It carries source extent, output origin/extent, scaled radius, edge/Alpha/quality flags and sampling origin/extent. The fixture performs edge-mapped identity sampling only. Public CPU blur/sharpen filters arrive in 0.13.0b; dedicated WGSL kernels remain disabled until their future parity suite approves them.

Straight RGBA is authoritative. `extractHalo()` preserves exact RGBA8/RGBA64 components, including hidden RGB at Alpha zero. Three explicit processing modes are available: independently filtered straight RGBA, straight RGB with source Alpha preserved, and coverage-aware premultiplied filtering with an independently filtered hidden-RGB fallback where output Alpha is zero.

The existing Shadows/Highlights adjustment remains the only production spatial adjustment in 0.13.0a. Its established fixed-cost 13-tap separable local-luminance kernel now lives behind the shared foundation; RGBA8 horizontal quantisation, exact 16-bit CPU behaviour, clamp edges, parallel large-image execution and the existing per-feature WGSL parity gate remain unchanged. Full-resolution and native tiled renderers now derive the same dependency plan, and composite tile revisions include its fingerprint.

Explicit halo materialisation is rejected above 512 MiB, deterministic CPU fixture working memory is bounded at 768 MiB, radii normalise to 0–4096 pixels, and all long loops observe an atomic cancellation flag. Invalid oversized plans fall back to the full existing source dependency rather than publishing a partial result.

No 0.13.0a data enters `.vfxphoto`, Hot/Warm/Cold snapshots, Undo history, presets, export profiles, production plans, queue snapshots or recovery descriptions. Project format 15, snapshot schema 16, colour-state schema 4, adjustment schema 10 and vector schema 7 remain unchanged.


## 0.13.0b blur/sharpen boundary

Gaussian Blur, Box Blur, Unsharp Mask and High Pass are ordinary `LayerType::Adjustment` nodes. Their enum values are appended after Shadows/Highlights, and adjustment schema 11 serializes only the new parameters. Project format 15, snapshot schema 16, colour-state schema 4 and vector schema 7 remain unchanged. Older adjustment schemas and enum identities are never renumbered or inferred.

Each filter reports a document-space support through `adjustmentSpatialRadius()`. The existing hierarchy traversal sums sequential/Pass Through supports and takes the maximum across isolated branches. The tiled compositor uses that cumulative support to expand the source dependency before `ImageProcessor` evaluates the stack, then crops back to the requested tile. Preview rendering scales the declared support and the actual kernel by the same document-to-preview ratio.

`gaussianBlurReference()` distributes the exact declared integer support over three separable box passes. Each pass is a sliding-window operation, so cost is linear in pixels rather than radius, and the pass supports sum exactly to the planned halo. Box Blur executes one such pass. Both can filter coverage-aware premultiplied RGBA with an independent straight-RGB zero-Alpha fallback, or preserve source Alpha while filtering straight RGB.

Unsharp Mask and High Pass derive detail from the same deterministic Gaussian approximation. Unsharp threshold is persisted in 8-bit code-value units and normalised identically for RGBA8/RGBA64. Both preserve source Alpha exactly. High Pass writes `0.5 + source - blur`, with an optional Rec.709 monochrome detail path; sharpening blend modes remain normal layer blend modes.

The new filters operate in `EncodedWorking`, so they do not introduce an ICC/OCIO/ACES domain conversion and cannot contaminate monitor, proof or export-output transforms. The independent RGB-reference render continues to preserve hidden colour under zero Alpha. Masks, group modes, selection-driven dirtiness, channels, history, residency, project save/load and immutable export/queue snapshots use their existing generic adjustment-layer paths.

The native compositor remains feature-approved per adjustment. IDs 16–19 are reserved, but 0.13.0b does not set their approval bits because dedicated WGSL kernels have not passed parity validation. Any hierarchy containing one of these filters therefore takes the deterministic CPU tiled path; unrelated approved hierarchies remain GPU eligible. This is an explicit safe fallback and preserves the established no-unvalidated-shader contract.

### 0.13.0b.2 CPU scheduling and interactive quality

The deterministic reference remains the authoritative fallback, but it no longer serialises its complete sampling footprint on one worker. `ImageProcessor` supplies a bounded `SpatialRowProcessor` backed by a dedicated `QThreadPool`; Box/Gaussian component extraction, horizontal rows, vertical columns and writeback execute over disjoint memory ranges. Unsharp Mask and High Pass detail composition use the same pool. Implicitly shared `QVector` and `QImage` storage is detached before dispatch, retaining the 0.11.0i.3 memory-safety rule.

During a continuous property gesture, Gaussian Blur and Box Blur may render the visible viewport from an existing preview mip chosen to keep the CPU footprint near 512×512 pixels. The document-space radius and halo scale through the existing preview contract. Unsharp Mask and High Pass are excluded from that quality reduction because downsampling removes the exact high-frequency information their controls modify. Project pixels, settled canvas output and every export/queue path remain full quality.

### 0.13.0b.3 interaction-frame lifetime

Reduced-resolution CPU spatial previews remain transient presentation tiles, but their lifetime now spans same-size content generations. Starting a newer slider generation retains the last complete tile rather than clearing it and exposing the older authoritative backing. The newer complete tile replaces the retained tile under the next validated generation/request serial. When the gesture ends, the retained mip stays visible while the atomic level-0 viewport renders; authoritative coverage masks it at commit, after which the transient record is removed. Size changes, channel-mask presentation and ordinary navigation generations still clear transient tiles immediately.

### 0.13.0b.4 detail-sensitive interaction policy

The selected adjustment determines the spatial interaction level. Gaussian Blur and Box Blur retain the responsive mip policy because loss of high-frequency detail is compatible with judging a blur. Unsharp Mask and High Pass force level 0 for the visible viewport, preserving hair, texture, edge halos and threshold behaviour throughout the gesture. These full-detail frames use the same one-worker coalescing and atomic complete-frame publication as 0.13.0b.3, so an older complete frame remains visible until the next authoritative frame is ready.

Mouse release no longer creates an unconditional extra render generation for a detail-sensitive interaction. If the latest level-0 generation already committed, it is immediately adopted as the settled result and normal thumbnail/residency bookkeeping resumes. If it is still queued or running, that same latest generation is allowed to finish and becomes final. Blur interactions still schedule their required level-0 release render because their live frame may be a mip.

## 0.13.0c colour-adjustment essentials

Identifiers 20 and 21 append Invert and Photo Filter without renumbering the established 0–19 adjustment contract. Adjustment schema 12 is the first schema permitted to contain these identifiers. Existing schemas continue through their historical parser branches, and a new identifier paired with an older schema is rejected. Public project format 15 and private snapshot schema 16 remain unchanged because both already embed versioned `AdjustmentData` objects.

Invert is a parameter-free point operator in `EncodedWorking`: straight RGB becomes `1 - RGB`, Alpha is copied, and hidden colour remains independently composited by the established RGB-reference path. It requires no managed-domain lattice and no spatial dependency.

Photo Filter is a point operator in `EncodedSrgb`. Its persisted colour is encoded sRGB. CPU and WGSL convert that colour to linear Rec.709, normalise it into a bounded optical transmission scale, optionally normalise the scale by Rec.709 luminance, apply the Density exponent, multiply the source linear RGB and gamut-limit the interpolation from source to filtered target. Managed documents enter and leave that domain through the existing adjustment lattice. Display/View, proof and export-output transforms remain outside the layer stack.

The C++/WGSL adjustment uniform gains one aligned `vec4` for Photo Filter colour and Density; Preserve luminosity uses the previously aligned discrete-parameter vector. The uniform remains 16-byte aligned with a compile-time size assertion of 640 bytes. Both operations run through ordinary per-adjustment parity approval, and Photo Filter additionally participates in the managed Display-P3 lattice test. Failure removes only the affected approval bit and selects the exact CPU reference.

Generic layer hashing includes Photo Filter colour, Density and Preserve luminosity. Invert has only its stable type identity. The existing adjustment-layer machinery therefore carries both through masks, group modes, Undo, projects, Hot/Warm/Cold, presets, Quick Export, Production Export, queue snapshots and recovery without adding another envelope field.


## 0.13.0d advanced-colour boundary

Colour Balance, Channel Mixer and Black & White retain their established numeric identifiers, serialized fields and output equations. Their Inspector controls continue to mutate ordinary `AdjustmentData` through one Undo gesture, but range/output switching now blocks programmatic slider signals so changing the viewed family cannot generate redundant renders or history writes. Channel Mixer exposes the selected RGB coefficient total as non-persisted UI feedback.

Selective Colour appends identifier 22. Adjustment schema 13 is the first schema allowed to contain that identifier and its nine fixed-order ranges: Reds, Yellows, Greens, Cyans, Blues, Magentas, Whites, Neutrals and Blacks. Each range stores Cyan, Magenta, Yellow and Black values in `[-100, 100]` plus one Relative/Absolute method flag. Schemas 1–12 cannot claim the new type; public project format 15 and private Hot/Warm/Cold schema 16 remain unchanged because their versioned layer payload already embeds `AdjustmentData`.

The CPU reference evaluates range membership from the original encoded RGB triplet. Primary families use the distance from the maximum component to the middle component; secondary families use the distance from the middle component to the minimum component. Whites and Blacks use the component range above or below 0.5, and Neutrals use the remaining bounded midtone span. Tied extrema deliberately activate adjacent families, producing continuous transitions at yellow, cyan and magenta boundaries.

Absolute mode applies direct process-colour percentage-point corrections. Relative mode scales those corrections by the process-colour amount already present, so pure specular white cannot acquire a process colour in Relative mode. Black is combined with each C/M/Y correction before clipping the resulting RGB delta to the component's available range. This contract is independently implemented in C++ and WGSL and is deterministic for RGBA8 and RGBA64 source components.

Selective Colour runs in `EncodedSrgb`, matching the established family-based colour operators. Managed ICC, OpenColorIO and ACES documents therefore use the existing working↔adjustment-domain lattices. The native compositor admits ID 22 only after both direct per-feature parity and managed-domain parity pass; otherwise the exact CPU reference is selected for that affected stack while unrelated approved features remain on GPU.

The adjustment uniform appends nine aligned `vec4<f32>` range records and one options vector, increasing the compile-time-checked block from 640 to 800 bytes. Tile cache identity hashes every range component and method. Generic layer, project, preset, residency, Quick Export, Production Export, queue and recovery paths require no new envelope fields. Alpha is copied unchanged and the independent hidden-RGB compositor remains authoritative.


## 0.13.0e optical and spatial colour-effect boundary

Vignette is a document-coordinate point operator evaluated in the established encoded-sRGB adjustment domain. RGB Split and manual Chromatic Aberration Correction are encoded-working-space spatial operators whose maximum signed channel displacement is reported to the shared halo planner, including one bilinear safety pixel. Their exact CPU references operate on straight RGBA8/RGBA64, preserve source Alpha, retain hidden RGB beneath zero Alpha, use clamped document edges, cooperate with cancellation and participate in cumulative stack dependency and cache fingerprints. They remain behind the per-adjustment GPU approval mask until dedicated WGSL kernels pass parity. No lens profile, camera metadata interpretation, Smart Layer or live-filter stack is introduced.

## 0.13.0f additional spatial-filter boundary

Surface Blur, Motion Blur and Radial Blur remain ordinary non-destructive adjustment layers. They consume the same layer accumulator, mask, opacity, blend-mode, group and Pass Through contracts as every prior adjustment; no Smart Layer or live-filter-stack state is introduced. Their stable append-only identifiers are 26–28 and adjustment JSON schema 15 is the first schema permitted to contain them.

The spatial dependency contract is now anisotropic. Each adjustment reports a document-space `QSize` halo, cumulative stacks add X and Y dependencies independently, Pass Through groups extend the parent dependency, and Isolated groups contribute the maximum independent branch. Motion Blur derives its halo from the projected half-distance plus bilinear safety; bounded radial blur uses the maximum centred half-span at the document edge. The existing scalar API remains as a compatibility wrapper returning the larger component.

Surface Blur is an edge-aware blend between the source and the established deterministic Gaussian reference. Threshold is stored in 8-bit code-value units and scales consistently in 16-bit processing. Motion and Radial Blur use bounded 4–64-sample bilinear references with clamped document edges. All three preserve straight hidden RGB. Surface Blur always preserves source Alpha; Motion and Radial Blur may either preserve source Alpha or diffuse coverage using the same premultiplied-colour reconstruction contract as Gaussian and Box Blur.

The new filters intentionally remain CPU-reference operations until dedicated WGSL kernels pass per-feature parity. Existing approved colour and tonal stacks remain GPU eligible. Cache fingerprints include all new parameters, stale generations observe cooperative cancellation, and export/queue snapshots capture immutable adjustment values through the existing generic layer serialization. Project, residency, preset-envelope, production-plan, queue and recovery schemas are unchanged.


## 0.13.0g existing-adjustment hardening boundary

0.13.0g adds no persisted adjustment type and does not change adjustment schema 15. Curves and Hue/Saturation on-image sampling are transient MainWindow/editor workflows: the active request is tied to the current layer and editor, cancelled on tool changes, Inspector rebuilds and document-session switches, and committed through the ordinary grouped property-Undo path.

The Hue/Saturation range visualisation mirrors the renderer's core-width plus smooth feather contract but remains presentation-only. Gradient Map duplication, distribution and keyboard operations edit the existing ordered stop vector and therefore use the established schema-15 payload unchanged.

Histogram calculation still renders the exact adjustment input first. Large input binning then partitions rows across a private pool capped at eight workers (six for 65,536-bin input), accumulates independent per-worker bins and reduces them in deterministic order. The private pool avoids nested starvation because the outer histogram request already runs on Qt's global concurrent pool. Cancellation remains cooperative and no partial histogram enters the cache.

## 0.13.0g.14 expanded-Vignette boundary

Adjustment JSON schema 16 appends one Vignette `size` scalar. The value is a uniform percentage applied to the document half-width, half-height and minimum half-span before the existing ellipse/circle blend and superellipse exponent. A value of 100 therefore preserves the schema-14/15 equation exactly; older Vignette payloads migrate to 100. Values up to 400 may place both the nominal radius and feather outside the document without increasing tile dependency because Vignette remains a point operator.

The selected Vignette's canvas guides are transient presentation state. `ImageCanvas` reconstructs the same inverse-radius blend, superellipse exponent, midpoint and feather distances as the CPU renderer, then draws the paths in document coordinates without clipping them to image bounds. Centre, Size, Midpoint and Rotation handles publish numeric parameters only; MainWindow applies them through the established grouped property-Undo and preview-coalescing path. The per-user visibility preference is stored in application settings, not the project, preset, session cache or export snapshot. In 0.13.0g.14.1, the transient canvas-to-MainWindow start/change/finish bridge uses explicit owner-cleared callbacks rather than adding new Qt meta-object signal symbols; the interaction and Undo contract is otherwise identical.

Generic adjustment serialization, preset payloads and tile fingerprints carry Size through existing containers. Public project format 15, private residency schema 16, production-plan, queue and recovery versions remain unchanged. The point kernel continues to preserve source Alpha and hidden RGB in both RGBA8 and RGBA64 processing.


## 0.13.0g.2 shared snapping boundary

The application owns one non-document `tools/snappingEnabled` preference. The
status-bar toggle and View action are synchronised front ends for that state;
the canvas still owns operation-specific algorithms. Guide drags quantise only
the actively moved coordinate to a 0.5 document-pixel lattice. Shape creation
and path-anchor gestures quantise only newly placed or actively moved anchor
coordinates to integer document boundaries. Existing guide/vector data is never
normalised during load, save, residency transitions or rendering.

Transform and Crop continue using their established target acquisition and
screen-space tolerance code, but their enable state is now supplied by the same
master preference. Bezier and Corner handles deliberately bypass the vector
anchor lattice. No project, adjustment, preset, queue, recovery or residency
schema changes are introduced.

## 0.13.0g.3 transform snapping and preview-preparation boundary

Whole-layer Transform gestures now apply a pixel lattice before magnetic target resolution. Translation rounds the resulting transformed top-left boundary to the integer document lattice, then accepts only target corrections that are whole pixels; this also repairs older fractional layer offsets while preserving compatible edge and centre alignment. Axis-aligned scale handles round the actively scaled document coordinate and consider only integer magnetic targets. Ctrl bypasses both stages. Rotated, skewed, distorted and perspective geometry is not destructively quantised.

Transform preview separation is no longer designed as mouse-press work. While the Move tool is idle with a valid whole-layer selection, MainWindow snapshots the current layer tree and asynchronously renders the selected-hidden background plus selected-only foreground through the normal RenderBackend/CPU-fallback contract. Results are accepted only when document-session identity, render serial, channel view and selected roots still match. A completed translation retains the unchanged background and derives the new selected foreground by applying the committed document translation, so repeated moves can begin from a valid cache without rerendering both surfaces. Cancellation and destructor waits preserve the established shutdown boundary; no worker captures MainWindow or live document references.

The cache is transient UI state only. It is never serialized, never enters Hot/Warm/Cold backing snapshots, and does not alter project, vector, adjustment, export, queue or recovery schemas.

## 0.13.0g.4 toolbar icon-asset boundary

Every selectable toolbar action and every parameterised vector-shape subtype now claims one dedicated resource path. `buildToolsToolbar()` tracks claimed paths while constructing the action families and reports missing or duplicate resources at startup, preventing silent blank buttons or future cross-tool icon reuse. The diagnostic is UI-only and does not participate in document state.

All toolbar artwork uses a 24 × 24 transparent RGBA PNG. `themedResourceIcon()` continues treating source Alpha as the icon mask and recolouring it from the active theme, so replacing an asset does not require theme-specific variants. Resource names are documented in `resources/icons/README.md` and compiled through the existing Qt resource collection. No project, vector, adjustment, preset, residency, Undo, export, queue or recovery schema changes are introduced.

## 0.13.0g.5 colour-pair control boundary

The colour dock keeps the primary and secondary swatches in one fixed 50 × 50 presentation widget. Both 30 × 30 swatches share an exact 16 px X/Y offset, leaving deterministic 18 × 18 top-right and bottom-left corner regions. Those complete corner regions are the hit targets for Reset and Swap; their `QToolButton` chrome remains transparent in normal, hover, pressed and focus states, so only the compact theme-aware icons are visible.

This is presentation-only state. Primary/secondary colour ownership, activation, double-click selection, persistence in `QSettings`, eyedropper behaviour, vector/brush colour consumption and project/preset formats are unchanged.

## 0.13.0g.15a non-destructive vector Feather foundation boundary

Feather is owned by `VectorLayerData`, not by individual `VectorShape` objects and not by `VectorAppearance`. One merged/imported vector layer therefore has one editable document-pixel Feather value across its complete fill-and-stroke silhouette, while each contained path/object retains independent geometry and fill/stroke styling. Text remains a separate `TextLayerData` model and is not advertised as vector geometry.

Vector-layer JSON schema 8 appends the explicit finite `featherRadius` scalar. Schemas 1–7 migrate to `0.0`; an older schema that dishonestly contains the new field, a schema-8 payload that omits it, and values outside `0.0…1,000,000.0` are rejected. `normalise()` does not clamp Feather, so persisted values are never silently changed. Generic layer/project, clipboard and residency serialization carry the field through the vector JSON envelope. Public `.vfxphoto` format advances from 15 to 16 and private Hot/Warm/Cold snapshots advance from 16 to 17. Older envelopes remain readable and migrate legacy vector payloads to `0.0`; non-zero Feather paired with an older outer version is rejected explicitly.

The Inspector uses `SliderSpinBox` with a `0.1 px` step and one decimal place. Its broad persisted range uses the control's large-range dynamic scrub sensitivity: ordinary dragging changes whole pixels near zero, Shift-drag permits tenths and exact typed values remain available. Start/change/finish signals enter the established property-Undo capture, update the live layer, invalidate vector/tiled cache fingerprints, schedule preview work and commit one history command.

Vector merge has one representability rule in this foundation: all selected vector layers must have the same Feather value. A matching value is copied to the merged editable path layer; differing values produce an explicit error instead of choosing a value or rasterising. Fill/stroke appearance presets remain shape-style payloads and deliberately preserve the independent layer-level Feather value.

The raster equation is unchanged in 0.13.0g.15a. Including Feather in vector and tiled fingerprints is safe placeholder integration for the upcoming renderer: `0.0 px` follows the accepted code path exactly, while non-zero edits update state and cache identity without being misrepresented as finished coverage feathering. Exact CPU rendering is reserved for 0.13.0g.15b, followed by native tiled GPU work in 0.13.0g.15c.

## 0.13.0g.15b exact CPU vector Feather renderer

At exactly zero, `VectorRasterizer` calls the accepted semantic fill/stroke renderer directly and stores that unchanged result under the existing cache contract. A non-zero value renders two compact source surfaces over the geometry that can affect the requested region: the ordinary authored appearance and an opaque silhouette generated from the same resolved fill and stroke paths. The silhouette uses the existing fill rule, compound contours, inside-stroke clip, expanded stroke geometry, dashes, caps, joins and arrowheads, so Feather does not invent a second vector interpretation.

Only silhouette Alpha enters the separable filter. The CPU kernel is the deterministic three-box Gaussian approximation already used by the spatial-filter foundation: integer support is distributed across three passes identically, while fractional document-pixel radii interpolate the exact adjacent integer kernels. Instead of materialising an empty halo, each one-dimensional convolution is evaluated from prefix moments and the closed-form three-box kernel. Cost therefore follows compact contributing geometry and requested output, not the distance between an off-canvas object and the document. Direct full-region requests whose estimated working set exceeds 512 MiB are recursively split into exact row-copied subregions. No persisted value is rounded or clamped.

The filtered silhouette is combined with the original appearance through a deterministic nearest-covered-pixel map. RGB is copied from one authored appearance pixel rather than convolved, while the local style Alpha is separated from geometric antialias coverage and multiplied by the feathered silhouette. This preserves semi-transparent fills/strokes, hard internal colour transitions and Alpha-safe channel rendering. Holes feather inward, exterior edges feather outward, and unchanged interior pixels retain their original straight RGB and style Alpha. The final surface is reconstructed as straight RGBA8 or RGBA64 and converted to the requested compositor format. The current vector schema exposes solid fill/stroke colours only; no gradient schema is invented or flattened in this renderer, and future semantic paint types remain isolated from the coverage filter because it consumes the ordinary appearance surface.

`contentBounds()` expands semantic vector bounds by the document-pixel Feather radius. Tile requests include every contributing source sample and use global preview coordinates, making stitched CPU tiles deterministic against a full-region render. This CPU path remains the authoritative fallback and parity reference for native execution.

## 0.13.0g.15c native tiled GPU vector Feather renderer

The native compositor still receives editable vector semantics rather than a rasterised layer asset. For non-zero Feather, `VectorRasterizer::prepareGpuFeatherTile()` resolves the compact source rectangle that can affect one requested tile, rasterises the ordinary authored appearance and the combined opaque fill/stroke silhouette through the accepted QPainter geometry path, and builds the same deterministic nearest-authored-colour/style-Alpha carrier used by the CPU reference. Geometry interpretation, fill rules, holes, open paths, stroke outlines, dashes and arrowheads therefore do not diverge between CPU and GPU.

A dedicated WGSL module performs only coverage convolution. The horizontal entry point reads silhouette Alpha and an uploaded exact fractional three-box X kernel into a float storage buffer. The vertical entry point applies the Y kernel and writes straight RGBA8 using carrier RGB and `featheredCoverage × styleAlpha`. Kernel weights are generated from the same integer support distribution and adjacent-support interpolation as the CPU reference. Source/output origins are explicit signed global preview coordinates, so negative off-canvas geometry and independently requested tiles evaluate the same samples.

GPU use is feature-gated, not assumed from device availability. Startup validation compares a synthetic fractional, holed, partially covered and semi-transparent carrier case against a CPU implementation of the same two-pass equation. A maximum channel difference above one leaves vector Feather on the exact CPU path while unrelated approved GPU features remain enabled. Supports above 256 preview pixels, source/output resource guards, 16-bit documents, allocation/pipeline/bind/readback failures and unavailable WebGPU all reject the complete native tile and trigger the exact renderer; no approximate interaction surface is published.

Tiled cache identity tests the current feather-expanded vector bounds against each dependency tile. Non-contributing tiles hash only a stable contribution marker, while contributing tiles hash world transform, vector fingerprint and layer revision. Geometry, transform and Feather edits therefore preserve distant cached composites, invalidate touched tiles, and also invalidate a tile when the expanded silhouette enters or leaves it. Ordinary masks remain separate compositor inputs. Project/vector/residency schemas are unchanged. Workflow-wide merge/rasterise/export/SVG decisions remain 0.13.0g.15d.


## 0.13.0g.15d vector Feather workflow integration

Feather remains a visual layer-appearance property, not geometry. Bounds-sensitive workflows that need rendered extent therefore call `VectorRasterizer::contentBounds(layer, worldTransform)`, which expands the resolved fill/stroke silhouette by the document-pixel Feather radius. Reveal All and Fit Canvas to Selected Layers use that rendered extent before their existing finite raster-mask intersection. Geometry editing, snap points and Inspector transform pivots deliberately continue using semantic vector bounds so a soft halo does not move anchors or alter transform centres.

Image Size is the one workflow that changes the document's pixel coordinate scale itself. Vector Feather therefore scales with the same minimum-axis factor used by the existing document-pixel stroke/radius contract. The stored value is validated against `MaximumFeatherRadius`; an unsafe result aborts the prepared resize rather than clamping persisted state. Ordinary layer Transform/rotation/scale leaves Feather numerically unchanged because it remains measured in document pixels around the transformed silhouette.

Expand Stroke must preserve the layer-level combined-silhouette boundary. For non-zero Feather, generated retained-fill and expanded-stroke objects stay in one vector layer, in their original render order, beneath the original transform/mask/opacity/blend state. Splitting them into isolated children would filter each component independently and is not equivalent. The accepted zero-Feather direct/group expansion path remains untouched. Editable Merge applies the same exactness rule more strictly: independently feathered source layers cannot generally become one vector layer with one Feather operation over their union, even when numeric radii match, so any non-zero Feather causes a clear rejection. Zero-Feather merges retain the existing exact editable conversion.

The shared compositor already places Feather at the correct boundary: `VectorRasterizer` produces the layer image; the ordinary raster mask is applied afterwards; then layer opacity/blend mode is composited. Isolated groups and Pass Through recursion consume that same result. Ctrl-click/selection coverage uses the renderer's feather-expanded bounds/image, and flattened Quick Export plus Production Export both use `ImageProcessor::renderPreservingHiddenRgb()`. Project format 16 and Hot/Warm/Cold snapshot format 17 already persist schema-8 vector state, so 15d adds no new envelope. The application currently has no standalone destructive Vector → Raster Layer command; existing explicit flatten/render paths all consume the authoritative renderer.

Standard SVG filters do not exactly represent Photo Lab's deterministic fractional three-box combined-silhouette kernel without changing editable semantics. SVG export therefore does not emit an approximate Gaussian filter and does not rasterise the layer. Exact VFX round-trip state remains in `data-vfx-vector-data`, with `data-vfx-feather-radius` exposed for diagnostics, and an export warning states that external SVG viewers receive the editable unfeathered vector geometry. Re-import into VFX Photo Lab restores the exact Feather from metadata.


## 0.13.0g.15e vector Feather hardening

The final hardening pass does not change the layer, project or residency schema and does not introduce another renderer. Exactly zero Feather still enters the accepted semantic `renderSemanticRegion()` branch. Regression coverage now verifies that editing away from zero and returning to `0.0` produces byte-identical RGBA8 and RGBA64 output to the original zero state. The existing compact-bound CPU evaluator is also exercised at `VectorLayerData::MaximumFeatherRadius` so large stored values remain mathematical support, not allocation dimensions.

The native GPU preparation preflight is intentionally conservative. Its 256 MiB budget now counts the semantic and coverage images, potential straight-format conversion detachments, prepared coverage storage, nearest-X scratch, the authored-colour carrier and small prefix/envelope overhead before those allocations occur. A rejection remains an honest whole-tile CPU fallback and never changes the persisted Feather value. The ordinary 256x256 compositor path remains comfortably below this guard.

Repeated Feather edits retain the established vector raster cache limits of 128 MiB and 2048 image entries; regression coverage drives many distinct layer revisions and checks those ceilings. Snapshot-version hardening also verifies that a container claiming residency format 16 cannot carry non-zero schema-8 Feather state. Vector schema 8, project format 16 and residency format 17 remain authoritative.

The current vector paint model is solid fill/stroke colour plus opacity. There is no separate editable vector-gradient paint payload to harden in this release, so 15e does not invent one or reinterpret the Raster Gradient Tool. All existing masks, groups, Pass Through compositing, SVG metadata, export, colour-management and residency paths continue to consume the shared Feather renderer established in 15b–15d.



## 0.13.0g.15e.2 Feather transform-proxy performance

Ordinary affine transforms of vector/text layers no longer synchronously regenerate semantic vector pixels on every pointer event. The transform session prepares one full-resolution foreground presentation and keeps it immutable while `ImageCanvas::updateTransformPreview()` advances the transform matrix, exactly as for raster layers. When the prepared foreground covers the document and is 8-bit, the existing parity-gated native GPU transform-preview compositor can handle rotate/scale cadence; otherwise ImageCanvas retains its established transformed-image fallback.

This also removes the former per-event `setTransformPreviewForeground()` reset. That setter performs `displayManagedCopy()` for the complete foreground, so repeatedly reinstalling a Feather-expanded image made even translation proportional to Feather surface area. Text-box resizing is intentionally excluded because changing a text box changes glyph layout and therefore still requires semantic rerasterisation. Apply stores only the semantic layer transform and invalidates the normal renderer; no vector raster proxy is persisted or baked.

## 0.13.0g.15e.1 Feather transform-preview performance

Live vector rotate/scale interaction is a transient semantic generation, not a persistent canvas generation. It therefore uses `RenderBackend::renderInteractiveRegion(..., skipPersistentCacheFallback=true)` for in-document foreground updates. This prepares one exact requested region and one Feather halo rather than independently rebuilding overlapping halos for every persistent 256×256 tile. Exact CPU fallback remains available behind the same call. Because these live images are already transformed, the foreground is retained as a compact image with explicit document bounds instead of being expanded to the full preview surface each pointer event. Translation keeps the existing cached-surface move fast path. Final document rendering still uses the ordinary tiled compositor and persistent cache.
