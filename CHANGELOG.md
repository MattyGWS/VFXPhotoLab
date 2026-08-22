# Changelog

## 0.14.0m.2 — Layer Effect Owner-Selection UX Fix

- Split presentation-row focus from owning-layer tool selection: Layer Effect and Live Filter rows resolve to hierarchy-safe owning roots for canvas/layer commands while remaining independently addressable sub-items in the Layers panel.
- Move/Transform, transform overlays/prewarm/snapping, selected-pixel transforms, vector/path commands, mask operations, duplication/grouping/merging, Smart Layer source commands, Fit Canvas to Selected Layers and Export Selected SVG now operate on the owning layer when an fx/filter row is focused.
- Preserved Layer Effect/Live Filter focus across whole-layer, selected-pixel and text-box transform commits so the Inspector stays on the edited effect/filter while its owner is moved or transformed.
- Kept Delete and Move Up/Down semantics scoped to the selected effect/filter row rather than its owner.
- Added the VFX Texture Lab-style automated Windows x64 release pipeline: one maintainer command can commit/push `main`, run the clean MSVC/Qt/wgpu-native/OpenColorIO build and tests on GitHub Actions, package portable and Inno Setup distributions, derive GitHub Release notes from this changelog, verify checksums/smoke reports, and publish the release.
- Fixed the first MSVC release-build blocker (`C2026: string too big`) by embedding the 44.8 KiB authoritative adjustment WGSL as conservative 8 KiB literals and reconstructing it once at runtime, preserving the shader byte-for-byte while staying below MSVC's per-literal limit.
- Fixed the second Windows release-build blocker in `VFXPhotoLabTests`: refreshed stale selection-edit signatures and clipboard/guide/layer field names, replaced non-existent Qt helpers (`QPainter::fillEllipse`, `QTransform::fromRotate`), and made 16-bit `QImage::fill` calls unambiguous under MSVC without changing the tested rendering semantics.
- Fixed the third Windows release-build blockers: `VFXPhotoLabCanvasTests` now links the authoritative `vfxphotolab_core` instead of compiling a drifting partial copy of core sources, while explicitly including its UI-only AppStyle/Curves/widget implementations; the Windows application target also uses MSVC `/bigobj` so the integrated `MainWindow.cpp` translation unit can exceed the default COFF section count without changing generated code or ABI.
- Hardened Windows CI test diagnostics after the first fully linked MSVC build reached CTest: all QtTest executables now use the Windows console subsystem so assertion/failure text reaches CTest, the workflow emits and archives `ctest-results.xml`, local diagnostic paths are ignored, and the one-command publisher removes any accidentally tracked `windows-build-failure-*` dump before staging the release commit.
- Hardened the next Windows CTest failure diagnosis without weakening the suite: every authoritative CTest invocation now forces QtTest's plain-text logger to a per-executable file, CI prints those first-run logs into the failed GitHub Actions step and archives them while preserving the original CTest exit status, and downloaded `windows-diagnostics-*` trees are now ignored and automatically untracked by the publisher as well.
- Windows Build Fix 6 addresses the first real cross-platform regression batch exposed by those logs: exact identity/tiled adjustment handling, current project/snapshot-schema test expectations, Windows-safe preset metadata overwrites, Export Queue recovery/Skip Existing semantics, Qt 6.8 ACES integer-working proxies, headless transform/document-strip tests, semantic premultiplied compositor comparisons, destructive-crop selection handling, legacy Smart/vector fixture downgrades, and an exact CPU fallback for selection-aware raster brush strokes until that hidden-RGB GPU path has its own parity approval.
- Windows Build Fix 7 addresses the remaining Fix 6 Windows regression set without weakening release gates: exact no-op adjustment preservation, platform-stable full-frame/tile comparisons, strict OCIO integer-working proxies only for document working-space conversion (not export destinations), safe CPU fallback for unapproved masked/fractional adjustment and Live Filter GPU coverage, semantic vector colour equality, deterministic Export Queue Skip Existing execution checks, headless transform gesture injection, corrected legacy embedded-Smart fixtures, proof-lattice CPU fallback acceptance, and stale vector/round-trip precision assertions aligned with the authored premultiplied rendering contract.
- Windows Build Fix 8 addresses the final four failing Windows suites from Fix 7: preserves fractional synthetic transform coordinates in headless Qt tests, honours pre-cancelled no-op adjustment renders, validates hidden RGB against an opaque spatial-filter reference, fixes the copied nested Pass Through fixture, compares vector persistence through its canonical serialised state, makes Vector Feather use canonical cross-depth RGBA64 coverage/style alpha and quantise only at final output, routes parity-rejected masked/fractional adjustment and Live Filter hierarchies through one exact CPU reference pass, and removes platform-specific QPainter stroke-vs-fill edge-pixel assumptions from Expand Stroke regression coverage.
- Windows Build Fix 9 addresses the two suites still failing in Windows run 32491995540 without relaxing release gates: uses QtTest-routed transform button events with platform-derived pointer coordinates, restores historically serialised non-Line vector endpoint state so equality/cache fingerprints survive save/load and shape conversion, keeps Vector Feather's high-precision alpha while sharing the exact RGBA8 authored-colour carrier with GPU preparation, evaluates the Live Filter parity regression through the document's managed render-session compatibility instead of the legacy convenience overload, converts v28 snapshot downgrade fixtures to the true pre-identity envelope layout, and makes the pre-version-13 Expand Stroke fixture explicitly carry the nonzero-winding feature it is intended to reject.
- Windows Build Fix 10 addresses the seven remaining assertions from Windows run 32532507427 without weakening release gates: drives the five fractional/composed transform-math regressions through the production ImageCanvas mouse handlers with exact viewport-space coordinates instead of platform-plugin synthetic-button rounding, gives the pre-Feather snapshot guard a dedicated first-layer Feather fixture so the v16 reader reaches the intended compatibility check before later per-layer fields, and makes RGBA8 Vector Feather colour-carrier selection use the same canonical RGBA64 coverage/style geometry as the 16-bit reference while preserving exact authored 8-bit RGB whenever it is representable. It also ignores and automatically removes accidentally tracked downloaded Windows diagnostic ZIPs before release commits.
- Windows Build Fix 11 repairs the Fix 10 MSVC compile regression without backing out its remaining-test hardening: the exact-coordinate Canvas transform tests now use a narrowly scoped friend test peer to invoke the real final ImageCanvas mouse handlers rather than illegally subclassing the final widget. Windows CI also captures the complete CMake/MSVC build stream into the diagnostics artifact so any future compile-stage failure includes its compiler output.
- Windows Build Fix 12 repairs the remaining MSVC compile blocker exposed by the new build log: the exact RGBA8 Vector Feather colour-carrier helper now propagates the converted semantic image colour space instead of referring to the pre-refactor `straight` identifier. No rendering algorithm, test tolerance, schema, or release behaviour is changed.
- Windows Build Fix 13 addresses the two suites still failing after the clean Fix 12 MSVC build without weakening release gates: Transform pointer gestures now use the same document-edge coordinate domain as transform bounds and viewport mapping instead of the pixel-centre `width-1`/`height-1` inverse that compressed every drag delta, Vector Feather now uses one shared RGBA8 half-code coverage threshold for canonical nearest-colour selection in both CPU output and GPU preparation, and Windows CTest diagnostics now dual-log QtTest output to stdout plus a retained `windows-ctest.log` so assertion details survive even if per-executable files are missing.
- No persistence/schema changes.

## 0.14.0m.1 — Layer Effect Immediate Refresh Fix

- Fixed newly added renderable Layer Effects not appearing on the canvas until a later property edit or unrelated refresh.
- Added matching immediate preview and document-thumbnail scheduling after Layer Effect add, remove and reorder operations.
- Kept the fix at the structural fx invalidation boundary; no persistence/schema changes.

## 0.14.0m — Full Workflow Integration and Hardening — 2026-08-14

- Hardened open-document linked Smart Source propagation with a runtime-only resolved document-identity closure: direct and transitive dependents update after a source save, while unrelated linked graphs are skipped instead of being re-opened/re-rendered globally.
- Kept that resolved identity closure non-persistent and safety-bounded, so project data remains authoritative and every Hot/Warm/Cold restore re-resolves the live graph.
- Added export preflight refresh for accepted Quick Export and Production Export operations so flattened and queued snapshots cannot silently use an out-of-date external `.vfxphoto` presentation. Cancelling export dialogs remains mutation-free.
- Rebuilt linked-source warning state after structural Undo/Redo, Smart Source registry adoption and Save/Save As path rebasing, fixing stale missing-link diagnostics after state restoration.
- Targeted linked refresh now preserves warnings belonging to other unresolved links and only advances source/dependent revisions for actually changed branches.
- Added regression coverage for targeted `A → B → C` propagation versus unrelated projects, runtime closure non-persistence, missing-link structural history restoration, and an integrated 16-bit Smart Transform + Live Filters + filter mask + Layer Effects + layer mask + opacity/blend save/reopen render round-trip.
- Corrected current project-format expectations in Smart Transform, Live Filter and Layer Effect persistence tests from 26 to 27 after the 0.14.0l format bump.
- No persisted schema changes: `.vfxphoto` remains **27**, Smart Source descriptor schema **3**, embedded Smart document schema **10**, Hot/Warm/Cold snapshot **28**, and Layer Effect schema **4**.

## 0.14.0l — Linked Smart Layers, Replace and Relink — 2026-08-14

- Added explicit external `.vfxphoto` Linked Smart Sources with persistent document UUID identity separated from relocatable path metadata.
- Added resolved SHA-256 source fingerprints covering both direct project bytes and nested linked-source fingerprints, so transitive linked updates invalidate correctly without global recomputation.
- Added **Place Linked Smart Layer**, **Replace Source**, **Relink Source**, **Embed Linked Source**, and linked-aware **Edit Contents** that opens/reuses the external project document.
- Saving a normal `.vfxphoto` now refreshes linked Smart Sources in all currently open dependent sessions, including Cold sessions restored from private backing snapshots.
- Added missing-link and identity-mismatch handling that preserves the last valid cached presentation where available and never silently selects another source.
- Legacy pre-format-27 projects derive their one-time migration identity from project bytes when available, so moving an untouched 0.14.0k.2 source still allows **Relink Source** to recognise it before its first 0.14.0l save persists the UUID.
- Added strict cross-file cycle rejection for authored links (`A → B → C → A`) plus bounded warning-based handling when an already-cyclic project is opened.
- Preserved instance-owned Smart transforms, Live Filters, Layer Effects, masks, opacity and blend modes across linked source refresh, Relink, Replace and Embed operations.
- Preserved authoritative source colour state by rendering the linked source in its own working space and converting only the derived presentation through the existing containing-document binding path.
- Added Save As link rebasing and protection against overwriting a linked source with the owning project.
- Added nested-link rebasing for embedded Smart Contents editors so linked sources inside embedded Smart documents resolve against the owning project instead of the process working directory.
- Added linked-source Inspector/Layers status, project-open warnings, Undo integration for explicit source-management operations and resident cross-document updates, and persistence/session-cache migration guards.
- Bumped public `.vfxphoto` format to **27**, Smart Source descriptor schema to **3**, embedded Smart document schema to **10**, and private Hot/Warm/Cold snapshot format to **28**. Layer Effect schema remains **4**.
- Added core regressions for linked persistence, missing links, Relink identity enforcement, Replace, Embed, per-instance state preservation, transitive nested invalidation and cross-file circular dependency rejection.

## 0.14.0k.2 — Fedora Build Fix 2 — 2026-08-14

- Fixed GCC/Qt 6 compilation of Layer Effect reordering by explicitly narrowing `QVector::size()` (`qsizetype`) to the existing `int` index domain before `std::clamp`.
- Fixed the identical Live Filter reordering compile failure.
- Retains all 0.14.0k.1 fixes; no rendering, project-format, embedded-schema, snapshot, or Layer Effect schema changes.

## 0.14.0k.1 — Fedora Build Fix — 2026-08-14

- Fixed `layerNodeFromJson()` compilation by qualifying the `LayerNode::MaximumLiveFilterCount` and `LayerNode::MaximumLayerEffectCount` static bounds used by the free JSON-loader helper.
- Added the missing `<QIODevice>` include required by `ImageProcessor.cpp` cache-key `QDataStream` construction on Fedora/GCC 16.
- No rendering, project data, Smart Layer, Live Filter, Layer Effect, colour-management, residency or export semantics changed. Public project format remains **26**, embedded Smart schema remains **9**, Hot/Warm/Cold snapshot remains **27**, and Layer Effect schema remains **4**.


## 0.14.0k — Bevel & Emboss — 2026-08-14

- Activated the core non-destructive **Bevel & Emboss** renderer on the separate per-layer `fx` stack.
- Added Layer Effect schema 4 authored controls for Inner Bevel / Outer Bevel / Emboss / Pillow Emboss, Up/Down direction, Depth, Size, Soften, light Angle/Altitude, and independent Highlight/Shadow colours, blend modes and opacities.
- Implemented Bevel geometry from full-precision mask-aware effective coverage using a signed distance field, a style-specific height profile, finite-difference normals and directional lighting instead of an offset-shadow approximation.
- Emit independent highlight and shadow effect passes so CPU and native hierarchy compositing preserve real Screen/Multiply/etc. semantics against the actual backdrop.
- Integrated Bevel spatial radius into existing Layer Effect halo propagation, dirty fingerprints, bounded 192 MiB effect-pass cache and native resident-texture reuse.
- Added bounded large-region Bevel streaming for full-resolution CPU export/recovery so signed-distance, height and two generated passes do not remain allocated at full-document scale simultaneously.
- Added exact 8-bit/16-bit, tiled-stitch, style/direction, Smart embedding, persistence and Hot/Warm/Cold regression coverage.
- Bumped public `.vfxphoto` format to **26**, embedded Smart document schema to **9**, private Hot/Warm/Cold snapshot format to **27**, and Layer Effect schema to **4**, with nested anti-smuggling migration guards.
- Schema-3 Bevel placeholders migrate disabled with deterministic 0.14.0k defaults, preserving old-project appearance while making later explicit enable useful.

## 0.14.0j — Stroke and Overlay Effects — 2026-08-14

- Activated exact non-destructive **Stroke**, **Colour Overlay** and **Gradient Overlay** renderers on the separate per-layer `fx` stack.
- Added Layer Effect schema 3 parameters for Stroke Position plus Gradient stops/interpolation/style/angle/scale/reverse, with safe schema-1/2 migration that leaves older placeholder definitions disabled while assigning usable 0.14.0j defaults.
- Implemented coverage-derived Stroke with Inside/Centre/Outside placement; Outside is an independent behind-source pass, while Inside/Centre composite above the source.
- Implemented mask-aware Colour Overlay and a full Gradient Overlay using the existing gradient-stop model, Linear/Radial styles, stable document-space owner bounds, Angle, Scale and Reverse.
- Anchored gradient evaluation to the owner layer rather than render-region coordinates, added exact full-region-vs-tiled stitch regression coverage, and conservatively hash global owner identity for Gradient Overlay because distant coverage-bound changes can alter its mapping.
- Integrated Stroke spatial radius into existing Layer Effect halo propagation, dirty fingerprints, bounded 192 MiB effect-pass cache and native resident-texture reuse.
- Kept CPU-generated effect passes independent through native WebGPU hierarchy composition so blend modes operate against the real backdrop without a lower-quality alternate effect renderer.
- Bumped public `.vfxphoto` format to **25**, embedded Smart document schema to **8**, private Hot/Warm/Cold snapshot format to **26**, and Layer Effect schema to **3**, with top-level and embedded anti-smuggling migration guards.
- Added focused regressions for all Stroke placements, overlay visibility, gradient stop variation/tiled stability, schema-2 placeholder migration, Smart embedding, persistence and session-envelope rejection.

## 0.14.0i — Shadows and Glows — 2026-08-13

- Activated exact non-destructive **Drop Shadow**, **Inner Shadow**, **Outer Glow** and **Inner Glow** renderers on the separate per-layer `fx` stack.
- Added Layer Effect schema 2 parameters for colour, independent effect opacity, blend mode, angle/distance, Spread/Choke and Size, with deterministic schema-1 migration and future-effect renderer guards.
- Generate effects from full-precision mask-aware owner coverage. Drop Shadow/Outer Glow composite behind source content; Inner Shadow/Inner Glow composite above it while remaining constrained to the effective masked silhouette.
- Added separable bounded max-filter Spread/Choke and exact Gaussian coverage processing with transparent-edge halos and 8/16-bit output paths.
- Added a bounded 192 MiB Layer Effect intermediate-pass LRU so painting underneath unchanged effects does not regenerate coverage/morphology/blur work on every parent-composite miss.
- Stream large full-resolution CPU renders one effect pass at a time when several fx are active, avoiding simultaneous full-canvas shadow/glow surfaces while retaining the shared multi-pass cache for normal tiled interaction.
- Made tiled composite dirty fingerprints effect-aware: source and mask dependencies expand by the effect halo, while disabled/future definitions contribute no render-cache identity.
- Kept generated effect passes independent through the native hierarchy so WebGPU composites their real Screen/Multiply/etc. modes against the actual parent backdrop. Effect passes also participate in resident-texture reuse; unsupported semantic generation is never replaced by a lower-quality fake GPU blur.
- Added exact off-canvas effect bounds, mask-aware halo propagation, colour-space tagging and grayscale-safe effect colour handling.
- Bumped public `.vfxphoto` format to **24**, embedded Smart document schema to **7**, private Hot/Warm/Cold snapshot format to **25**, and Layer Effect schema to **2**, with anti-smuggling migration guards.
- Added focused regressions for 16-bit masked shadow generation/final composition, 8-bit glow depth, inner/outer pass placement, parameter persistence, embedded Smart preservation and old-envelope rejection.

## 0.14.0h — Layer Effects Architecture and `fx` UX — 2026-08-13

- Added a distinct persistent ordered `LayerEffect` stack with stable UUID/revision state on Raster, Vector, Text and Smart Layers; it is intentionally separate from Live Filters and Adjustment Layers.
- Added the complete core effect-definition vocabulary: Drop/Inner Shadow, Outer/Inner Glow, Stroke, Colour Overlay, Gradient Overlay and Bevel & Emboss. Definitions are created disabled until their exact renderer revision lands, preventing silent no-op effects.
- Added Layers-panel `fx  Layer Effects` containers and effect pseudo-rows, Inspector/context-menu add/remove/reorder UX, and protections that keep pseudo-rows out of structural layer drag/drop and layer-only commands.
- Added the exact effect-input contract: unclipped hidden-RGB-preserving pre-effect content plus separate mask-aware 8/16-bit coverage, independent of layer opacity/blend.
- Added Layer Effect render-identity, future halo-radius and resident/Undo memory-accounting hooks for later tiled effect renderers.
- Preserved Layer Effect definitions through structural Smart conversion, embedded Smart contents, Undo/Redo, public project persistence and Hot/Warm/Cold snapshots.
- Advanced `.vfxphoto` to **23**, embedded Smart document schema to **6**, private snapshot format to **24**, and introduced Layer Effect schema **1**, with strict old-envelope anti-smuggling.
- Added focused regression coverage for the fx stack model, disabled-renderer guard, mask-aware coverage contract, embedded Smart preservation, project migration and Cold snapshot round-tripping.

## 0.14.0g — Live Filter UI, Masks and GPU Integration — 2026-08-07

- Exposed per-Smart-instance Live Filters as dedicated Layers-panel pseudo-rows plus Smart Layer **Add Live Filter** menus, with enable/disable, remove, reorder and mask thumbnails while protecting the structural layer tree from pseudo-row drag/drop or layer-only commands.
- Reused the complete 0.13 adjustment/filter Inspector editors through a Live Filter binding, including grouped slider/curve Undo and spatial-interaction preview scheduling.
- Added schema-2 per-filter masks in Smart-instance reference space with create/from-selection, enable/disable, invert, remove and **Load Selection → edit → From Selection** authoring. CPU and native paths share the exact transformed Smart-mask sampler.
- Added Live Filter-aware Levels/Curves histogram input and Levels/Curves/Hue-Saturation/White-Balance on-image sampling against the exact pre-target filter prefix.
- Added parity-gated native WebGPU Live Filter execution by reusing startup-approved adjustment kernels inside an isolated Smart subgroup. Stacks containing any unapproved operator use the exact CPU Live Filter reference and remain eligible for existing Smart tile native residency/composition.
- Preserved Live Filter prefix-stage caching and avoided whole-stack invalidation while scrubbing; filter masks and render semantics participate in cache/native residency identity.
- Advanced `.vfxphoto` to **22**, embedded Smart document schema to **5**, private Hot/Warm/Cold snapshots to **23**, and `LiveFilter` schema to **2**, with strict anti-smuggling migration for mask metadata.
- Added regressions for masked Live Filter semantics/persistence, pre-filter histogram analysis, native CPU/GPU parity where approved, migration and residency.

## 0.14.0f — Live Filter Stack Foundation — 2026-08-07

- Added a persistent ordered Live Filter stack on Smart Layer **instances**, with stable UUIDs, enabled state, monotonic revisions and typed `AdjustmentData` payloads. Live Filters remain distinct from Adjustment Layers, Smart Sources and future Layer Effects.
- Added document-model operations for add/remove, enable/disable, parameter updates and reorder. Shared Smart Source instances retain independent Live Filter stacks.
- Added exact CPU stack execution at **Smart Source → instance transform → Live Filters → instance mask → opacity/blend**, reusing the existing 0.13 adjustment/filter algorithms and colour/precision contracts.
- Added cumulative X/Y spatial halo propagation in transformed parent space, including off-canvas dependency requests so filtered Smart content can influence visible pixels across the document edge.
- Added prefix-stage Live Filter caching through the existing bounded `SmartLayerTileCache`: changing a downstream filter preserves transformed-source and unchanged upstream stage results, while unrelated lower-layer edits can reuse the complete unchanged stack.
- Added Live Filter semantics to tile-local composite fingerprints and native Smart resident keys. Disabled-filter parameter edits remain non-contributing until enabled.
- Kept native execution honest in this foundation revision: CPU-reference filtered Smart tiles feed the existing WebGPU hierarchy compositor/residency path; native per-filter GPU kernels remain 0.14.0g work.
- Added Live Filter dynamic payloads to document/Undo residency estimates, including LUT tables, curve points and gradient stops.
- Advanced public `.vfxphoto` to **21**, embedded Smart document schema to **4**, and private Hot/Warm/Cold snapshot format to **22**, with strict old-envelope anti-smuggling and empty-stack migration.
- Added focused regressions for stack order, toggle, add/remove, parameter edits, lower-layer/prefix-cache reuse, persistence, migration, embedded schema and session snapshot round-tripping.

## 0.14.0e — Tiled Smart Rendering and Cache Architecture — 2026-08-07

- Added a bounded Smart intermediate cache with 256×256 straight-RGBA64 source tiles and reusable transformed parent-space tiles, independent of the final composite tile cache.
- Added transform-aware inverse source-footprint requests with Bicubic/Lanczos halo propagation so overlapping high-quality output tiles share source conversion work instead of repeatedly preparing whole presentations.
- Cached transformed Smart instance masks alongside transformed content, preventing mask resampling when only composition underneath changes.
- Unified native hierarchy Smart-mask preparation with the exact CPU Smart-mask sampler/cache, including Bicubic/Lanczos support halos, so GPU composition does not substitute a generic smooth mask transform.
- Made transformed Smart cache identity content-aware through exact sampled source-region fingerprints. Unchanged output footprints can survive an unrelated Edit Contents revision bump without being flushed by the global source revision alone.
- Made final tiled composite hashing Smart-contribution-local: transforms outside a tile do not invalidate it, and source dirty propagation hashes only the exact source overlap that the output tile can sample.
- Added native WebGPU Smart-tile residency reuse through the existing bounded resident-texture LRU. The hierarchy compositor borrows an already-resident Smart tile instead of re-uploading unchanged intermediates after lower-layer edits.
- Added selective source-cache lifecycle rules: changed revisions remain safely content/version addressed and age out by LRU; removed source identities are explicitly purged.
- Added a 192 MiB default RAM budget for Smart source/transformed image intermediates plus a separately capped source-region fingerprint metadata cache.
- Integrated Smart intermediates with Hot/Warm/Cold residency: Cold eviction purges the session's Smart RAM identities, and native Smart residency keys are tracked under the document session so renderer-session release also frees their VRAM entries.
- Kept Smart cache state runtime-only; public project format remains **20**, embedded Smart schema **3**, and private Hot/Warm/Cold snapshot format **21**.
- Added focused regressions for lower-layer repaint reuse, transformed-mask reuse, cross-revision far-edit reuse, near-edit invalidation, tile-local Smart transforms and source dirty propagation.

## 0.14.0d — Non-Destructive Smart Transforms — 2026-08-07

- Added persistent per-Smart-instance transform sampling state while keeping authoritative Smart Source contents/presentations unmodified by Apply. Repeated scale/rotation/affine operations therefore resample from the current source instead of from an intermediate transformed raster.
- Existing 0.14.0a-c Smart Layers migrate to Bilinear sampling to preserve their historical compositor result; new transformed Smart instances retain the Transform tool's selected Nearest/Bilinear/Bicubic/Lanczos 3 method. Translation-only moves do not unexpectedly change sampling quality.
- Added exact straight-RGBA64 Bicubic/Lanczos/Nearest Smart transform evaluation using the shared transform reference sampler, preserving hidden RGB beneath zero Alpha.
- Added matching persisted Smart-mask sampling so content and instance masks remain registered under high-quality transforms.
- Bounded exact CPU preparation to the inverse-mapped source footprint plus interpolation halo instead of converting the whole Smart presentation for every requested region.
- Kept live transform interaction on the existing immutable foreground proxy/matrix-only update path; semantic Smart rendering occurs after Apply rather than on each pointer event.
- Added Smart transform sampling to structural equality/Undo identity and tiled composite cache hashing.
- Added embedded Smart document schema 3 for nested Smart transform metadata, public `.vfxphoto` format **20**, and private Hot/Warm/Cold snapshot format **21**, with migration defaults and anti-smuggling guards for older envelopes.
- Added regressions covering 20%→80% repeated source-backed transforms, authoritative-source immutability, transformed source edits, sampling persistence/migration, nested schema guards, masks and session snapshot persistence.

## 0.14.0c — Smart Layer Edit Contents and Source Dependencies — 2026-08-07

- Added **Edit Smart Layer Contents** to the Layer menu/context workflow and Smart-thumbnail double-click. Embedded sources open as ordinary multi-document `DocumentSession`s rather than a restricted special editor.
- Added persistent source-editor owner/source bindings with baseline source revisions. The binding survives private Hot/Warm/Cold session snapshots.
- Added branch-style source editing: owner Smart Source state is unchanged until Save successfully validates and commits the edited source document.
- Added transactional Smart Source commit with stale-owner detection, nested-source adoption, monotonic revision propagation, dependency-first dependent refresh, and circular dependency rejection.
- Added structural parent-document Undo/Redo for successful Smart source commits, including updates into inactive resident owner sessions.
- Added recursive owner/source-editor close handling so nested contents documents cannot outlive their authoritative owner sessions.
- Added embedded Smart Source schema 2 with explicit authoritative **8-bit/16-bit** precision metadata. Public `.vfxphoto` format is now **19**; format-18/schema-1 embedded sources remain readable and precision metadata forged into a pre-v19 envelope is rejected.
- Bumped private Hot/Warm/Cold snapshot format to **20** and added migration guards for schema-2 precision payloads.
- Added a colour-managed Smart presentation boundary: authoritative embedded source pixels remain in the source working space while the parent-facing derived presentation is converted to the containing document working space. OCIO/ACES document state continues using the existing exact Qt working-space proxy contract where required by QImage storage.
- Added transactional presentation rebinding to Smart conversion, structural layer insertion/replacement, Smart Source replacement and structural state restore so a failed working-space bind cannot leave partially mutated document state.
- Added runtime Smart presentation buffers to resident/Undo memory accounting when colour conversion creates a distinct parent-space image.
- Added focused regressions for nested B→A source propagation, source cycle rejection, schema-1→schema-2 migration guards, 16-bit sRGB/Display-P3 composition preservation, embedded Smart cold snapshots, and Smart source-editor binding snapshots.

## 0.14.0b — Smart Layer Embedded Contents and Conversion — 2026-08-07

- Added authoritative embedded Smart Source document payloads that preserve the selected `LayerNode` trees rather than flattening them. Embedded state carries layer IDs/order, raster/vector/text/adjustment/group data, masks, transforms, colour-management state, canvas metadata and nested Smart references.
- Added **Convert to Smart Layer** to the Layer menu and Layers context menu for single layers, groups and contiguous same-parent multi-selections. Conversion is prepared atomically and MainWindow records one structural Undo command restoring the original parent tree and Smart Source registry exactly.
- Added a revision-keyed exact source presentation cache owned once by each Smart Source. Smart Layer instances bind to that shared presentation at runtime and remain source-ID references rather than owning unrelated rendered copies.
- Added exact Smart rendering through the existing compositor, including an unclipped hidden-RGB-preserving reference path for derived Smart presentations, meaningful transparent-RGB bounds, masks, off-canvas content and 8/16-bit processing.
- Bounded the interim whole-source presentation to 64 Mpix / 256 MiB uncompressed; unsafe larger conversions fail cleanly until 0.14.0e replaces this cache with tiled source requests.
- Added conservative appearance preflight: backdrop-dependent top-level adjustments, non-Copy blends and Pass Through groups are converted only when their complete lower sibling backdrop is included in an isolated/transparent context; unsafe selections are rejected instead of silently changing appearance.
- Added nested Smart Source dependency extraction, deep embedded-payload validation, stale nested-source rejection and source-presentation rebinding after Undo/Redo, project load and Hot/Warm/Cold restore.
- Added Smart presentation thumbnails in the Layers panel. Direct painting remains unavailable on Smart Layers; **Edit Contents** is intentionally the next 0.14.0c workflow.
- Bumped public `.vfxphoto` format from 17 to **18** and private session snapshots from 18 to **19**, retaining older-format loading while rejecting embedded Smart payloads forged into pre-0.14.0b envelopes.
- Added focused conversion/persistence/hidden-RGB/structural-restore regression coverage and raised the bounded Smart Source cold-snapshot envelope for real layered embedded documents.

## 0.14.0a — Non-Destructive Layer Architecture and Smart Source Foundation — 2026-08-07

- Added an explicit Smart Layer instance type without changing ordinary Raster layers into source-backed layers. Each Smart instance stores a persistent source ID/revision reference alongside its existing transform, mask, opacity and blend state.
- Added a document-owned `SmartSourceRegistry` with UUID source identities, monotonic revisions, Embedded/Linked storage intent, bounded dependency metadata and transactional acyclic-graph validation.
- Added transitive revision invalidation: editing a source advances only that source and the Smart Sources that depend on it, then synchronises only instances referencing affected source IDs. Source deletion is rejected while another source or layer still depends on it.
- Added source registry state to structural Undo/Redo snapshots and document replacement, preventing history from restoring Smart Layer instances without their matching source graph.
- Bumped public `.vfxphoto` format from 16 to 17 and persisted the Smart Source registry plus per-Smart-Layer source reference. Versions 1–16 remain readable and cannot smuggle Smart state into older schemas.
- Bumped private Hot/Warm/Cold session snapshots from 17 to 18 and added exact registry/reference persistence with graph validation on write and restore.
- Added Smart source ID/revision to tiled composite hash identity. No whole-document reprocessing path, flattened Smart preview, or silent low-quality substitute is introduced.
- Added focused project/session round-trip, cycle rejection, dependency-cascade and linked-descriptor safety regressions. Embedded source document contents, conversion and Edit Contents remain the next focused revisions rather than being approximated in 0.14.0a.

## 0.13.0g.15e.2 — Feather Transform Proxy Performance — 2026-08-07

- Removed synchronous semantic vector/Feather rerasterisation from ordinary Move/Rotate/Scale pointer events. Editable vectors now use the same immutable full-resolution transform-surface proxy used by raster layers while the gesture is active.
- Routed ordinary vector affine preview updates through `ImageCanvas::updateTransformPreview()`, allowing in-document 8-bit rotate/scale gestures to use the existing native GPU transform-preview compositor instead of the live-vector CPU path.
- Removed per-event `setTransformPreviewForeground()` resets. Those resets regenerated the complete display-managed foreground copy and made even pure translation increasingly CPU-bound as Feather expanded the prepared surface.
- Kept text-box resizing on semantic live rerender because it changes layout rather than applying an affine layer transform.
- Final Apply remains non-destructive: the vector transform is stored semantically and the authoritative Feather renderer republishes the settled result. No persistence schema changed.

## 0.13.0g.15e.1 — Feather Transform Preview Performance — 2026-08-07

- Fixed vector rotate/scale transform interaction becoming progressively slower as per-layer Feather increased.
- Routed live semantic transform rerenders through the exact bounded single-submit interactive compositor, avoiding repeated overlapping Feather-halo preparation across persistent 256×256 tiles.
- Kept transformed live-vector foreground surfaces compact instead of expanding them to the complete preview image on every pointer event.
- Added regression coverage proving the transient bounded compositor matches the tiled CPU reference for a large fractional Feather while leaving the persistent tile cache untouched.
- Kept vector schema 8, project format 16 and Hot/Warm/Cold snapshot format 17 unchanged.


## 0.13.0g.15e — Hardening and Regression Coverage — 2026-08-06

- Completed the vector Feather mini-milestone with focused regressions for exact zero-Feather restoration, tiny geometry, the full 1,000,000 px stored radius, fill/stroke combinations, dashed/cap/join variants, winding rules, RGBA8/RGBA64 coverage and bounded repeated scrub revisions.
- Corrected the native GPU preparation memory preflight so its existing 256 MiB guard conservatively includes semantic source images, format-conversion temporaries, prepared coverage, nearest-colour scratch and the output colour carrier before allocation.
- Added a private cold-session compatibility regression that rejects non-zero Feather when a snapshot dishonestly claims the pre-Feather version-16 envelope.
- Retained exact CPU fallback, GPU parity gating, masks/groups/Pass Through, off-canvas rendering, Expand Stroke/Merge rules, SVG round-trip compatibility, export paths and Alpha-safe hidden RGB.
- Kept vector schema 8, public project format 16 and Hot/Warm/Cold snapshot format 17 unchanged. No 0.14.0 Smart Layers work is included.

## 0.13.0g.15d — Compositing and Workflow Integration — 2026-08-06

- Integrated rendered vector Feather bounds into Reveal All and Fit Canvas to Selected Layers, preserving visual halo extents for transformed and off-canvas vector geometry while keeping transform pivots/node editing tied to semantic geometry.
- Scaled per-layer Feather during Image Size by the established minimum-axis document scale used for vector pixel-radius appearance values, with explicit overflow rejection instead of silent persisted-value clamping.
- Made Expand Stroke lossless for feathered layers by retaining generated fill/stroke outline components in one editable vector layer under the original layer transform, mask, opacity and blend state. Zero-Feather expansion keeps its accepted group/direct-replacement behaviour.
- Corrected editable Merge Layers semantics: every non-zero vector Feather is rejected because separately feathered source silhouettes cannot generally be represented by one feathered merged union, even when radii match. Zero-Feather vector merges remain exact.
- Audited compositor ordering for vector opacity/blend modes, raster masks, Isolated and Pass Through groups, selection-derived coverage, multi-layer transforms, project/session persistence and flattened exports. These already consume the authoritative Feather renderer and require no parallel implementation.
- Added explicit SVG compatibility handling. Exact Feather remains in `data-vfx-vector-data` and is also exposed as `data-vfx-feather-radius`; export warns that standard SVG viewers receive editable unfeathered geometry because no standard SVG filter exactly represents Photo Lab's combined-silhouette three-box kernel. No approximate Gaussian filter or silent rasterisation is emitted.
- Added focused regressions for mask/group compositing, Feather-aware canvas bounds, editable SVG round-trip, non-zero Merge rejection, Feather-preserving Expand Stroke and Image Size scaling.
- Kept vector schema 8, project format 16 and Hot/Warm/Cold snapshot format 17 unchanged. Smart Layers remain 0.14.0; vector Feather hardening remains 0.13.0g.15e.

## 0.13.0g.15c — Native Tiled GPU Feather Rendering — 2026-08-06

- Added a dedicated two-pass WGSL coverage convolution for non-zero vector-layer Feather. Semantic fill/stroke geometry, holes, winding, dashes, caps, joins and arrowheads remain rasterised through the accepted editable vector path; only the combined silhouette coverage is filtered on the GPU.
- Matched the authoritative 0.13.0g.15b fractional three-box equation by uploading exact blended X/Y kernels, evaluating horizontal coverage into a storage buffer and vertically reconstructing straight RGBA8 through the exact nearest-authored-colour carrier. Fill/stroke RGB is never convolved.
- Added a startup GPU/CPU parity gate covering fractional anisotropic supports, internal holes, partial antialias-style coverage, varied authored RGB and semi-transparent style Alpha. The native path is approved only at a maximum channel difference of one.
- Integrated Feather into the tiled native hierarchy and interactive single-submit path. Failure, unavailable WebGPU, unapproved parity, 16-bit documents, supports above the approved bound or resource-guard rejection all use the complete exact CPU fallback; no low-quality preview or temporary Feather disablement is introduced.
- Added correct scaled halo/source preparation for off-canvas geometry and tile-global coordinates. Feather/geometry/transform cache identity is local to the feather-expanded silhouette, so unrelated distant tiles remain reusable while tiles entered or left by an edit are invalidated.
- Added focused regression coverage for the GPU input contract against the CPU reference and for distant tile-cache reuse after a Feather revision.
- Kept vector schema 8, project version 16 and Hot/Warm/Cold snapshot version 17 unchanged. No preset, export-plan, queue, recovery, colour-management or Smart Layer schema changed.

## 0.13.0g.15b — Exact CPU Vector Feather Rendering — 2026-08-06

- Added the exact non-destructive CPU reference renderer for per-vector-layer Feather without changing vector, project, residency, preset, queue or recovery schemas.
- Preserved the accepted byte-identical semantic vector raster path at exactly `0.0 px`; non-zero values now rasterise one opaque combined fill/stroke silhouette, feather only that coverage, and composite the unchanged authored appearance through it.
- Kept fill and stroke RGB out of the filter equation. A deterministic nearest-covered-pixel propagation step supplies halo colour without averaging neighbouring fills, strokes or overlapping objects, while style opacity and semi-transparent vector colours remain intact.
- Used the same QPainterPath fill rules, compound contours, stroke outlines, inside clipping, dashes, caps, joins and arrowhead geometry as the accepted vector renderer, preserving holes, winding and open-path behaviour.
- Added an exact separable three-box coverage kernel matching the established CPU Gaussian support distribution. Fractional document-pixel values interpolate between adjacent exact integer supports instead of being rounded or changing persisted state.
- Evaluated the compact-support convolution directly from alpha prefix moments, so very large Feather values and wholly off-canvas vector geometry do not require allocating an empty radius-sized halo. Oversized direct full-region calls split into bounded exact subregions, while requested tiles remain independent and stitch exactly to a full-region CPU render.
- Added straight RGBA8/RGBA64 output reconstruction, Alpha-safe opaque-channel rendering, cancellation checks, conservative feathered content bounds and cache identity through the existing vector fingerprint.
- Added focused CPU regressions for the `0 px` path, fill-plus-stroke colour separation, fractional and very large values, tiled/full equality, 8/16-bit behaviour, even-odd holes, open paths with arrowheads, off-canvas geometry and cancellation.
- Native tiled WGSL Feather remains intentionally reserved for 0.13.0g.15c; the current renderer is the authoritative CPU reference and fallback.

## 0.13.0g.15a — Vector Feather Data Model and Inspector Foundation — 2026-08-06

- Added a per-vector-layer `featherRadius` property measured in document pixels, defaulting exactly to `0.0`.
- Advanced `VectorLayerData` from schema 7 to schema 8, public `.vfxphoto` projects from version 15 to 16 and private Hot/Warm/Cold snapshots from version 16 to 17. Older vector payloads migrate to Feather `0.0 px`; schema-8 payloads require a finite explicit value between `0.0` and `1,000,000.0 px`, and invalid persisted values are rejected rather than silently clamped.
- Added a Vector layer appearance section to the Inspector using the established combined numeric scrub/value field, one-decimal exact entry and a `px` suffix.
- Routed scrubbing and typed edits through the existing grouped property-Undo path, live preview scheduling, vector-cache fingerprint invalidation, layer-tree refresh and document-thumbnail refresh.
- Preserved Feather through project serialization, private residency/cache JSON, clipboard/vector copies, ordinary duplication and inserted vector-layer copies. Project versions 1–15 and snapshot versions 2–16 remain readable, while a non-zero Feather value paired with an older outer format is rejected explicitly.
- Preserved matching Feather values through editable vector merges and rejected mixed-Feather merges with an explicit compatibility error instead of silently choosing or discarding a value.
- Confirmed that fill/stroke vector appearance presets do not reset the independent layer-level Feather value.
- Added focused core regression coverage for defaults, schema migration, dishonest metadata, safety bounds, fingerprints, duplication, copies, preset preservation and merge rules.
- Kept the accepted vector rasterizer unchanged for this foundation task. `0.0 px` therefore remains pixel-identical; exact CPU coverage feathering begins in 0.13.0g.15b.
- Made no rasterisation, Smart Layer, live-filter, adjustment, colour-management, mask/group, export, queue, recovery or shutdown-lifetime changes.

## 0.13.0g.14.1 — Vignette Overlay Linkage Fix — 2026-08-06

- Corrected the production linker failure in 0.13.0g.14 where `ImageCanvas.cpp` and `MainWindow.cpp` referenced three new Vignette overlay signals but the generated MOC object contained no matching signal bodies.
- Replaced only that new signal bridge with explicit start/change/finish callbacks registered by `MainWindow`. The callback path preserves the same grouped property Undo, live parameter updates, Escape cancellation and settled Inspector rebuild.
- Clears the callback bridge at MainWindow teardown, matching the lifetime safety formerly supplied by QObject connection ownership.
- Updated the outside-document Size-handle regression to verify callback delivery without depending on new MOC symbols.
- No Vignette geometry, adjustment schema, project/preset serialization, renderer, Alpha, 8/16-bit, mask/group, colour-management, residency, export, queue or recovery contract changed.

## 0.13.0g.14 — Expanded Vignette Geometry and On-Canvas Controls — 2026-08-06

- Traced the border-darkening limitation to Vignette geometry being permanently normalised against a document-sized ellipse. Added a uniform persisted Size value in the range 10–400%; 100% retains the previous equation exactly, while larger values place the nominal radius and falloff outside the document.
- Advanced only adjustment JSON to schema 16. Vignette data loaded from schemas 14–15 receives Size 100%, while generic project, preset-envelope, Hot/Warm/Cold, production-plan, queue and recovery containers retain their existing versions.
- Added matching CPU render, region-render and tiled-cache fingerprint support for Size, preserving straight hidden RGB, source Alpha, masks, group modes, Pass Through, 8/16-bit processing and cancellation.
- Added presentation-only Vignette geometry to `ImageCanvas`: faithful superellipse guides for the midpoint/start, nominal radius and feather/end, plus direct centre, Size, Midpoint and Rotation handles. Handles may lie and remain interactive outside the document in the surrounding canvas.
- Routed one complete on-canvas gesture through the existing grouped property-Undo path. Live values request ordinary adjustment previews; release guarantees the established settled result. Escape restores the gesture's initial parameters without adding an effective history change.
- Added a remembered per-user Show on-canvas controls switch and a built-in Corners Only preset. Overlay visibility is not serialized into projects and no guide enters exported pixels.
- Added focused coverage for legacy Size migration, corner-only output, region parity, 8/16-bit behaviour, preset self-containment and an outside-canvas Size-handle interaction.
- Smart Layers and Live Filters remain 0.14.0; no unrelated transform, colour-management, residency, export or recovery contract changed.

## 0.13.0g.13 — Off-Canvas Rotation Preview Retention — 2026-08-06

- Traced the remaining disappearance to transform preparation after an applied raster rotation. Rotation is correctly baked into compact editable raster/mask storage with a document-space reference origin, but the next preview rendered selected content into a document-sized surface, clipping any baked pixels whose bounds lay outside the canvas.
- Added a bounded transform-foreground snapshot carrying both its rendered presentation and explicit document-space bounds. In-canvas work retains the existing full-document equal-extent surface, while selected content partly or wholly beyond the document uses a compact surface without changing authoritative layer storage.
- Added an exact unclipped CPU region compositor for off-canvas transform preparation, including spatial-filter dependency halos, masks, isolated/Pass Through groups, 8/16-bit precision and processing-compatibility state. Ordinary in-canvas transform preparation continues to use the native tiled/GPU region renderer with the existing exact fallback.
- Extended `ImageCanvas` transform preview and preview-commit placement to draw arbitrary-sized foreground surfaces at their real document bounds. The full-document native GPU transform-composite path remains enabled only when foreground/background extent contracts match; compact off-canvas surfaces use the exact QPainter presentation path.
- Kept the 0.13.0g.12 accumulated-translation cache intact, now retaining the compact foreground bounds alongside the source image and base placement transform.
- Added core coverage that renders compact raster storage wholly beyond the source rectangle and canvas coverage proving the retained baked-rotation payload returns during a later transform and commits identically to its live preview.
- No project/preset schema, authoritative transform maths, rotation Apply/Cancel workflow, pure-move auto-commit, Undo grouping, colour-management, residency, export or recovery contract changed.

## 0.13.0g.12 — Off-Canvas Move Preview Retention — 2026-08-06

- Traced disappearing Move previews to `rebuildMovedTransformPreviewCache()`, which rasterised every completed translation into a document-sized image. Pixels translated beyond the document were permanently absent from that cached image, even though the authoritative raster layer and its translation still retained them.
- Kept the pre-move foreground surface unchanged and added a separate accumulated foreground-placement transform to the transform preview cache. Consecutive Move gestures now compose their translation onto that placement rather than repeatedly clipping the pixels through canvas bounds.
- Applied the accumulated placement consistently to the direct QPainter preview, native GPU transform-preview matrix and preview commit path.
- Preserved the existing pure-translation workflow: releasing the pointer commits the move immediately as one `Move Layer(s)` Undo command, while Apply remains reserved for genuinely pending scale/rotate/skew/distort/perspective work.
- Removed the obsolete document-clipping `transformedPreviewSurface()` helper.
- Added a regression test that retains a foreground wholly outside the right document edge, moves it back into the canvas in a following gesture and verifies live/committed pixel equality.
- No project schema, raster storage, mask, group, selection, hidden-RGB, 8/16-bit, colour-management, GPU/CPU or export contract changed.

## 0.13.0g.11 — Complete Tool Options Scrub Frames — 2026-08-06

- Used the Fedora screenshots to confirm that the remaining defect was not toolbar placement: the 28 px scrub widget had a complete top frame but no bottom scanline, while the same 30 px control in side panels rendered both borders.
- Traced the final two-pixel loss to Qt's style-sheet box model. Tool Options requested a 28 px physical widget and also styled the `QDoubleSpinBox` with a 28 px content height; its 1 px top and bottom borders therefore required a 30 px painted box and the lower frame was clipped.
- Kept the wrapper and physical spin-box geometry at 28 px, but set the toolbar-only styled content box to 26 px so the two border pixels fit exactly. The reusable 30 px Colour, Inspector, Layers and adjustment presentation is unchanged.
- Retained the invariant 44 px Tool Options bar, ruler/canvas position, field widths, text/suffix alignment, progress fill, spin buttons, typed entry and normal/Shift/Ctrl scrubbing.
- Upgraded the focused regression from geometry-only checks to rendering the real application stylesheet and asserting that the centre of the final scanline is the theme border colour rather than the input background.
- Made no project, preset, history, crop/transform, rendering, colour-management, residency, export, queue, recovery or shutdown-lifetime changes.

## 0.13.0g.10 — Tool Options Field Geometry and Shutdown Safety — 2026-08-06

- Traced the remaining Tool Options clipping to conflicting vertical contracts: the reusable scrub wrapper and native spin box were fixed at 30 px while the toolbar's ordinary buttons, combo boxes and spin boxes were normalised to a 28 px action row. KDE Breeze could clip the lower spin-box border when the 30 px child was constrained by the toolbar action geometry, particularly with device-pixel-ratio rounding.
- Added an explicit control-height API to the reusable scrub field, retained the comfortable 30 px default for Colour, Inspector, Layers and adjustment panels, and assigned all Tool Options scrub fields a matched 28 px wrapper and physical spin-box height. This removed the outer child/action mismatch but did not yet account for Qt style-sheet content-box height plus borders.
- Kept the Tool Options toolbar exactly 44 px high, preserved the ruler/canvas position, current widths, text alignment, progress fill, typed entry and normal/Shift/Ctrl scrub behaviour.
- Traced the Fedora exit abort to `DocumentSession` destruction: `QUndoStack::~QUndoStack()` emitted `cleanChanged`, the still-connected MainWindow callback entered `updateWindowTitle()`, and document-strip selection was mutated while the active session/member graph was being torn down.
- Added an explicit MainWindow teardown barrier that marks shutdown active, disconnects every session stack from MainWindow, removes all stacks from QUndoGroup, clears session-signal registration and blocks title/document-strip refresh re-entry.
- Added focused canvas/widget geometry tests for the 28 px toolbar scrub contract and the disconnect/remove-before-destroy Undo-stack ordering.
- Made no project, preset, adjustment, crop, transform, colour-management, GPU/CPU, export, queue or recovery schema changes.

## 0.13.0g.9 — Crop Rotation Undo and Angle Scrubbing — 2026-08-06

- Traced the apparent failed Crop Undo to structural history capturing the pending Straighten angle and crop frame as part of the pre-Apply document state.
- Confirmed that Undo restored the original pixels correctly, then immediately re-applied the captured non-zero angle through the still-active Crop preview; because pending angle edits are not separate history commands, repeated Undo could never return the field to zero and earlier edits remained visually obscured.
- Split Crop Apply state into an exact submitted snapshot used for asynchronous stale-result validation and a settled full-canvas history snapshot used by Undo/Redo.
- Centralised the post-Apply Crop state contract so Apply, Undo and Redo use a full-canvas frame, original canvas ratio, inactive Straighten sampling and a `0°` angle while retaining persistent Crop mode, overlay, dim, snapping and destructive-crop preferences.
- Replaced Transform's plain unbounded angle spin box with the existing combined scrubber/value field, bounded to `-180°…180°` with 0.1° steps and exact typed entry to match Crop.
- Added focused regression coverage for the settled Crop history contract.
- Made no project, preset, colour-management, rendering, export, queue, recovery or adjustment schema changes.

## 0.13.0g.8 — Incremental Live Paint Compositing — 2026-08-06

- Traced live raster and retouch strokes beneath adjustment stacks to the persistent tiled composite path, where each pointer update rebuilt cache identity by copying and hashing contributing layer pixels and rounded small dirty regions out to complete 256 × 256 tiles.
- Routed transient brush, eraser, tone/detail, smudge, clone, healing and patch preview composites through the existing single-submit native WebGPU hierarchy path.
- Added an explicit transient fallback mode that bypasses persistent tile-cache lookup and insertion and renders the exact bounded CPU region directly for 16-bit documents, unavailable GPU devices and parity-gated adjustment stacks.
- Retained the established tiled GPU/CPU fallback for large non-paint interactive viewport requests, so adjustment-slider and gradient previews still have a bounded resource-pressure path.
- Preserved spatial dependency halos, masks, selections, isolated and Pass Through groups, editable channels, Alpha-safe hidden RGB, final-quality post-stroke rendering and one-stroke Undo grouping.
- Added deterministic regression coverage verifying that transient CPU output matches the normal tiled compositor while leaving persistent cache resident/dirty/hit/miss statistics untouched.
- Made no project, adjustment, preset, colour-management, residency, export, queue or recovery schema changes. Crop rotation and the remaining Tool Options field clipping were inspected but intentionally left for focused follow-up revisions.

## 0.13.0g.7 — Numeric Field Layout and Inspector Stability — 2026-08-05

- Standardised bounded Tool Options scrub fields to one 112 px width instead of allowing uncapped expanding controls to consume the remaining toolbar space.
- Gave the reusable scrub field and its internal spin box a stable 30 px height and removed excess vertical spin-box padding so the progress fill and text occupy the full usable field height.
- Kept the 44 px Tool Options toolbar contract unchanged while aligning the taller combined fields within it.
- Prevented live layer-opacity updates from rebuilding the Inspector on every drag tick, eliminating the repeated vertical-scrollbar appearance/disappearance and avoiding unnecessary widget churn.
- Retained live compositing, grouped Undo, adjustment controls, exact-entry geometry fields, project/preset schemas and all CPU/GPU rendering behaviour.

## 0.13.0g.6 — Fixed Tool Options and Scrubbable Numeric Fields — 2026-08-05

- Gave the Tool Options toolbar an invariant 44 px layout contract and fixed size hints so switching tools cannot move the rulers or canvas vertically.
- Reworked the reusable bounded numeric control into one compact Krita-style field with a theme-aware progress fill, typed entry, double-click select-all and horizontal scrubbing.
- Added Shift-drag fine adjustment, Ctrl-drag coarse adjustment, drag-threshold protection for ordinary clicks, Escape restoration and safe completion when mouse capture or window focus is lost.
- Preserved one grouped property-Undo transaction per scrub or typed edit and guaranteed the final value is published when the interaction ends.
- Applied the combined control to existing adjustment parameters, colour RGB/HSV channels, layer opacity and suitable Tool Options values, including brush controls, vector appearance, Transform snap distance and Crop dim/straighten controls. Exact geometry, document dimensions and unbounded signed values remain direct-entry fields.
- Added signed-range zero-centred progress rendering and adaptive scrub sensitivity for ordinary and very large technical ranges.
- Preserved project/preset schemas, renderer paths, masks, selections, Alpha/hidden RGB, groups, colour management, residency, export and recovery behaviour.

## 0.13.0g.5 — Aligned Colour Pair Controls — 2026-08-05

- Aligned the primary and secondary colour swatches with equal horizontal and vertical offsets.
- Repositioned Reset and Swap into the exact top-right and bottom-left negative-space wedges around the colour pair.
- Made both actions visually chrome-free while retaining the whole 18 × 18 gap as their click target.
- Replaced the platform-dependent swap character with a compact theme-aware icon and retained the existing default black/white icon.
- Preserved all primary/secondary colour behaviour, preferences, accessibility and project compatibility.

## 0.13.0g.4 — Dedicated Tool Icon Assets — 2026-08-05

- Gave every selectable toolbar tool and every vector shape subtype its own dedicated 24 × 24 RGBA PNG resource under `resources/icons/`.
- Added visible placeholder assets for Line and Arrow, replacing their references to a missing `transform-skew.png` resource.
- Stopped vector Rectangle, Rounded Rectangle, Ellipse, Polygon, Star, Pen and Direct Selection tools from borrowing selection or Transform icons.
- Renamed the Transform and Clone Stamp resources to explicit tool-specific filenames and added a documented replacement map in `resources/icons/README.md`.
- Normalised the existing Blur and Sharpen resources to the same 24 × 24 canvas as the rest of the toolbar.
- Added build-time validation and startup diagnostics for missing, malformed or accidentally shared toolbar icon resources, without changing tool behaviour, projects, presets, rendering or persistence.

## 0.13.0g.3 — Transform Pixel Snapping and Gesture Responsiveness — 2026-08-05

- Extended the global Snap state into whole-layer Transform gestures: raster, vector, text and group moves now align the resulting bounds to integer document coordinates, and axis-aligned resize handles finish on integer pixel boundaries.
- Retained document, guide, visible-layer and vector-point magnetic targets while rejecting half-pixel corrections that would move layer edges off the pixel lattice; Ctrl remains the temporary free-movement bypass.
- Removed the repeatable start-of-drag pause by asynchronously prewarming the selected-only foreground and selected-hidden background transform surfaces while the Move tool is idle.
- Reused prepared surfaces directly at gesture start and rebuilt a translated foreground cache after committed moves, avoiding synchronous full preview separation on every drag.
- Preserved semantic vector/text live previews, native GPU rendering with honest CPU fallback, exact project/preset schemas, Alpha/hidden RGB, masks, selections, groups, Pass Through, 8/16-bit processing, colour management, residency, Undo and export/recovery contracts.
- Added deterministic canvas coverage for whole-pixel layer movement, Ctrl bypass and integer-boundary resize results.

## 0.13.0g.2 — Global Snapping and Pixel-Aligned Geometry — 2026-08-05

- Added one persistent **Snap** toggle in the bottom status area, synchronised with **View → Enable Snapping** and `Ctrl+Shift+;`.
- Removed duplicate Transform and Crop Snap checkboxes while retaining Transform's screen-space Snap Distance control.
- Quantised new and moved guides to a half-pixel document lattice so guides can target both pixel edges and pixel centres, including the exact centre of odd-sized documents.
- Pixel-aligned parameterised vector shape previews/commits, new Pen anchors and Direct Selection anchor drags/nudges to integer document boundaries.
- Kept Bezier and Corner handles subpixel-precise, skipped half-pixel centre guides as vector-anchor targets, and left existing saved geometry untouched until actively moved.
- Added deterministic snapping-lattice and 45-degree pixel-boundary regression coverage.
- Preserved project/preset schemas, rendering, colour management, Alpha/hidden RGB, residency, Undo/history, export queues and every 0.13.0 adjustment contract.

## 0.13.0g.1 — Bottom Status Zoom Controls — 2026-08-05

- Replaced the passive bottom-right zoom readout with compact Zoom Out, current percentage, Zoom In, 1:1 and Fit controls.
- Added predictable navigation stops: 25% boundaries from 25% through 3200%, with 6.25% and 12.5% fractional stops retained for very large images and the existing 2% minimum preserved.
- Zoom Out selects the previous stop and Zoom In selects the next stop even when the current zoom is already exactly on a boundary; for example, 183% moves to 175% or 200%, while 175% moves to 150% or 200%.
- Kept button zoom anchored to the visible canvas centre, reused the established Actual Pixels and live Fit-to-View paths, and left mouse-wheel zoom unchanged.
- Added compact theme styling, tooltips, accessible names, no-focus button interaction and disabled boundary/no-document states.
- Added deterministic canvas regression coverage for ordinary, exact-boundary, fractional, minimum and maximum zoom stops.
- Preserved project/preset formats, rendering, colour management, Alpha/hidden RGB, residency, Undo/history, export queues and every 0.13.0 adjustment contract.

## 0.13.0g — Existing Adjustment Improvements and Hardening — 2026-08-05

- Added a Curves on-image sample-point workflow for the selected master/component channel, with duplicate suppression, grouped Undo, exact editor synchronisation and one-shot tool restoration.
- Added a visual Hue/Saturation target-range strip, on-image centre sampling, independent range reset and range-selection persistence across Inspector rebuilds.
- Added Gradient Map stop duplication, even distribution and keyboard selection/nudging/deletion/colour editing.
- Added Curves Reset All and expanded built-in Curves, targeted Hue/Saturation and Gradient Map presets.
- Parallelised exact large-image 8/16-bit histogram accumulation on a bounded private worker pool, retaining deterministic reduction, selection coverage, managed luminance, cancellation and cache semantics.
- Centralised cancellation of one-shot Levels, Curves, Hue/Saturation and White Balance eyedroppers across tool changes, Inspector rebuilds and multi-document session switches.
- Added deterministic editor and histogram regression tests.
- Extended the native chained-adjustment GPU/CPU parity case with nontrivial Curves and Gradient Map operators.
- Preserved adjustment schema 15 and every project, residency, colour, vector, preset, export, queue and recovery contract. 0.13.0 is now complete for testing.

## 0.13.0f — Additional Spatial Filters

- Added Surface Blur as append-only adjustment ID 26 with Radius and a stable 0–255 edge threshold, deterministic Gaussian support, preserved source Alpha and hidden-RGB-safe processing.
- Added angle-aware Motion Blur as ID 27 with Distance, Angle, adaptive bounded sampling and optional transparency/Alpha diffusion.
- Added bounded Spin/Zoom Radial Blur as ID 28 with Amount-at-edge, adjustable optical centre, adaptive sampling and optional transparency/Alpha diffusion.
- Extended spatial dependency planning and tiled compositor cache contracts from scalar radii to anisotropic X/Y halos, including cumulative Pass Through and Isolated-group accounting.
- Added multicore straight-RGBA 8/16-bit references, bilinear clamped-edge sampling, cooperative cancellation and full-resolution detail-sensitive Surface Blur interaction.
- Added Inspector controls, built-in presets, Undo, project/preset/Hot-Warm-Cold/export snapshot integration and deterministic schema, halo, region, Alpha, bit-depth and cancellation tests.
- Advanced adjustment JSON to schema 15 without changing project format 15, Hot/Warm/Cold schema 16, colour schema 4, vector schema 7, export profiles, production plans, queue descriptions or recovery envelopes.

## 0.13.0e.1 — Optical and Spatial Colour Effects Build Fix

- Fixed the Fedora/GCC build failure caused by a duplicate anonymous-namespace `smoothStep()` definition in `ImageProcessor.cpp`.
- Reused the existing shared helper for Shadows/Highlights and Vignette rather than keeping a second identical implementation.
- Preserved all 0.13.0e effect behaviour, adjustment IDs 23–25, schema 14 and every project, residency, preset, export, queue and recovery contract.

## 0.13.0e — Optical and Spatial Colour Effects

- Added Vignette as adjustment ID 23 with document-relative geometry, Amount, Midpoint, Roundness, Feather, Centre, Rotation, Highlight protection, tint and inverted centre/edge behaviour.
- Added creative RGB Split as ID 24 with independent Red/Blue offsets, Green anchoring, bilinear clamped-edge sampling and radius-aware tiled dependencies.
- Added manual radial Chromatic Aberration Correction as ID 25 with signed channel edge shifts, adjustable optical centre and radial falloff.
- Added exact multicore 8/16-bit CPU references preserving straight Alpha and hidden RGB, with cooperative cancellation and parity-gated GPU fallback.
- Added Inspector controls, built-in presets, Undo, cache fingerprints, project/preset/residency/export snapshot integration and deterministic schema/region tests.
- Advanced adjustment JSON to schema 14 without changing project format 15, Hot/Warm/Cold schema 16, colour schema 4, vector schema 7, export profiles, production plans, queue descriptions or recovery envelopes.

## 0.13.0d — Advanced Colour Controls

- Added Selective Colour as append-only adjustment ID 22 with nine colour/tonal families, four CMYK-style controls per family, and Relative/Absolute correction methods.
- Implemented matching straight-RGB CPU and WGSL paths, per-feature parity approval, managed-domain parity coverage and chained compositor diagnostics.
- Advanced only adjustment JSON to schema 13; older adjustment IDs and project/residency/export/queue/recovery envelopes keep their existing meanings.
- Preserved Alpha and hidden RGB and added 8/16-bit, region/full-frame, managed Display-P3 and serialization tests.
- Expanded built-in Colour Balance, Channel Mixer, Black & White and Selective Colour presets.
- Hardened Colour Balance and Channel Mixer range switching against synthetic slider updates and added a Channel Mixer output-total readout.
- Added Selective Colour to tile cache identity, Hot/Warm/Cold snapshots, generic project/preset/export snapshots and immutable queued export state.

## 0.13.0c — Colour Adjustment Essentials — 2026-08-04

- Added non-destructive Invert and Photo Filter adjustment layers with append-only identifiers 20 and 21.
- Added Photo Filter Warming 85/LBA/81, Cooling 80/LBB/82 and Sepia choices, custom colour, Density and Preserve luminosity controls.
- Defined Invert as straight encoded-RGB complement with exact Alpha and hidden-RGB preservation.
- Defined Photo Filter in encoded-sRGB/linear-Rec.709 with bounded optical transmission, optional luminance normalisation and managed working-space round trips.
- Added matching WGSL paths, ordinary per-feature parity cases and managed Display-P3 Photo Filter parity validation.
- Preserved existing Vibrance, Threshold and Posterise identifiers, rendering contracts, presets and 8/16-bit behaviour.
- Advanced adjustment JSON from schema 11 to schema 12 only for the two appended types; dishonest schema-11 payloads using them are rejected.
- Extended tile-cache hashing, Inspector controls, built-in presets, project/session restoration and immutable export/queue snapshots through the existing generic adjustment paths.
- Added deterministic tests for identifiers, serialization, legacy rejection, 8/16-bit Invert, hidden RGB, Photo Filter identity/tint/luminosity behaviour, full-frame/region equivalence, presets, WGSL publication and Cold restoration.
- Kept project format 15, Hot/Warm/Cold schema 16, colour-state schema 4, vector schema 7, export-profile, production-plan, queue and recovery schemas unchanged.

## 0.13.0b.4 — Detail-accurate Sharpen interaction previews — 2026-08-04

- Kept Gaussian Blur and Box Blur eligible for the responsive document-scaled interaction mip while forcing Unsharp Mask and High Pass slider previews to full-resolution level 0.
- Fixed the misleading sharpen interaction image where mip downsampling removed fine detail and made the canvas appear blurred until mouse release.
- Preserved complete-frame atomic publication and previous-frame retention, so full-detail sharpen previews do not reintroduce the 0.13.0b.2 flicker.
- Reused the latest completed level-0 sharpen generation on mouse release instead of automatically scheduling a duplicate final render of identical parameters.
- Added deterministic preview-level policy coverage for detail-sensitive and blur-style spatial interactions.
- Preserved filter arithmetic, final pixels, adjustment identifiers and all project, preset, residency, export, queue and recovery schemas.

## 0.13.0b.3 — Flicker-free spatial interaction previews — 2026-08-04

- Fixed blur/sharpen slider flicker caused by clearing each reduced-resolution interaction tile before its replacement generation was ready.
- Retain the last complete CPU spatial interaction frame across same-size preview generations and replace it atomically with the next complete frame.
- Keep the final interaction frame visible while the level-0 release render runs, then remove the transient mip only after the authoritative viewport commits.
- Added a canvas regression test covering interaction-generation retention, replacement and final authoritative handoff.
- Preserved 0.13.0b.2 parallel CPU scheduling, deterministic final pixels and every project, preset, residency, export, queue and recovery schema.

## 0.13.0b.2 — Blur and Sharpen performance hardening — 2026-08-04

- Parallelised exact Box/Gaussian horizontal and vertical sliding-window passes across the bounded image-processing pool instead of evaluating every visible tile on one CPU core.
- Parallelised straight/coverage-aware component extraction and writeback plus Unsharp Mask and High Pass detail composition, with QImage/QVector detach completed before workers touch disjoint memory.
- Added mip-scaled interactive previews for large visible regions containing the CPU blur/sharpen filters; releasing the gesture still performs the unchanged level-0 authoritative render.
- Retained deterministic output, tile-halo equivalence, cancellation, Alpha-safe hidden RGB, 8/16-bit behaviour and the existing final export/queue/recovery paths.
- Added a row-scheduler regression fixture that executes work in reverse order and verifies byte-exact Box/Gaussian results.
- Preserved all adjustment identifiers and project, residency, colour, vector, preset, export, queue and recovery schemas.

## 0.13.0b.1 — Blur and Sharpen build fix — 2026-08-04

- Fixed the Fedora/Qt 6 build blocker in `SpatialFilter.cpp` by including the complete `QColorSpace` type before copying image colour-space metadata.
- Preserved all 0.13.0b adjustment identifiers, schemas, rendering contracts, presets and project/export/queue/recovery compatibility.

## 0.13.0b — Blur and Sharpen Essentials — 2026-08-04

- Added public Gaussian Blur, Box Blur, Unsharp Mask and High Pass adjustment layers with grouped creation menus and Inspector controls.
- Added append-only adjustment schema 11 serialization, default names, string identifiers, legacy migration coverage, tile-cache hashing and built-in presets for all four operations.
- Added a deterministic three-pass box approximation for Gaussian Blur; cumulative kernel support exactly matches the radius reported to the shared tiled halo planner.
- Added coverage-aware or source-Alpha-preserving blur, Alpha-preserving Unsharp Mask, neutral-grey High Pass and optional monochrome High Pass while retaining straight hidden RGB.
- Defined Unsharp threshold in 8-bit code-value units and scale it consistently for 16-bit documents.
- Kept all new spatial adjustments behind the existing per-feature GPU approval gate, selecting their bounded deterministic CPU references until dedicated WGSL kernels pass parity validation. Unrelated approved stacks remain GPU eligible.
- Added cancellation, 500 px public radius bounds, linear-time sliding-window passes, preview-radius scaling and cumulative multi-adjustment halo accounting.
- Added deterministic schema, enum-identity, Gaussian fixture, Alpha/hidden-RGB and full-frame-versus-tiled regression tests.
- Preserved project format 15, snapshot schema 16, colour-state schema 4, vector schema 7 and all existing project/export/queue/recovery interpretations.

## 0.13.0a — Spatial Filter Foundation — 2026-08-04

- Added a reusable, versioned spatial-filter contract with independent document-space X/Y radii, Clamp/Mirror/Wrap/Transparent edges, straight-RGBA Alpha contracts, preview quality, deterministic fingerprints and bounded safety padding.
- Added tile planning for sampling halos, neighbouring dependencies, crop offsets, inverse dirty-region expansion and cache invalidation.
- Added a stable 64-byte C++/WGSL uniform contract plus an edge-mapped WGSL fixture for future live-filter kernels.
- Added exact RGBA8/RGBA64 halo extraction, hidden-RGB preservation, cooperative cancellation and large-allocation guards.
- Added deterministic Box Blur reference fixtures covering full-frame/tiled equivalence, edge modes, Alpha modes, tile boundaries and 8/16-bit consistency; no public Box Blur is exposed yet.
- Reused the foundation for the existing parallel Shadows/Highlights 13-tap CPU kernel while retaining the established per-feature WGSL parity gate and CPU fallback.
- Centralised full-resolution and native tiled spatial dependency planning and included spatial-plan identity in composite tile cache revisions.
- Preserved project, residency, colour, adjustment, vector, preset, export, queue and recovery schemas.

## 0.12.0g — Integration and Hardening (2026-08-04)

- Adds one strict resolved-output validation boundary shared by queue acceptance and recovery persistence, checking enabled plan rows, stable IDs, captured profiles, resizing, naming, destination containment, encoding settings and writer availability.
- Rejects tampered or internally inconsistent queue payloads before history mutation or recovery-file creation.
- Allocates collision-checked queue UUIDs and persists a new job atomically before evicting any bounded terminal history, making failed enqueue attempts side-effect free.
- Rechecks Skip Existing against the live filesystem during execution instead of treating dialog-time skip metadata as permanent.
- Counts all configured outputs in queue progress, including deliberate skips, so finished jobs report complete progress consistently.
- Classifies a preserve-on-quit worker as complete only when every expected output completed or was deliberately skipped; fully finished jobs are finalised and their recovery records removed instead of being replayed next launch.
- Replaces recursive recovery-layer counting/ID validation with a bounded iterative traversal that also enforces maximum hierarchy depth.
- Requires straight RGBA8/RGBA16 queue snapshots and matching safe colour-processing contracts at both queue and persistence boundaries.
- Adds recovery-disk free-space preflight and explicitly disables direct-write fallback for recovery, raster/TGA export and unified preset writes, preserving the intended atomic publication contract.
- Makes managed preset-directory containment case-insensitive on Windows without changing Linux paths.
- Adds deterministic production-payload tampering, canonical auto-rename, duplicate destination, strict worker-completion, deep-layer, recovery re-resolution, progress and live Skip-policy execution tests.
- Completes the 0.12.0 Presets and Production Export Foundation milestone without changing project format 15, Hot/Warm/Cold schema 16, colour-state schema 4, adjustment schema 10 or vector schema 7. Folder-wide batch processing and automation remain 0.19.0.

## 0.12.0f — Recovery and Session Persistence (2026-08-04)

- Adds versioned, atomic private recovery snapshots for every accepted unfinished Export Queue job.
- Serialises the immutable source image with exact 8/16-bit straight RGBA storage, including hidden RGB beneath transparent Alpha, plus the complete layer tree, colour state, processing contract and production-export plan.
- Restores valid snapshots at startup in a new non-terminal **Recovered** state that never auto-starts or writes files.
- Adds **Resume Recovered** with fresh profile/path/resize/naming/writer validation and explicit confirmation for current Ask-before-replace collisions.
- Recalculates Skip and Auto-Rename decisions against the current filesystem when a recovered job is resumed.
- Adds a three-way close workflow: preserve unfinished jobs for the next launch, cancel them permanently, or keep the application open.
- Quarantines malformed, oversized, wrong-schema, mismatched-contract and compatibility-repaired recovery files with bounded startup warnings.
- Deletes private recovery files when jobs reach a terminal state or are explicitly cancelled, while cleanly preserved jobs survive orderly shutdown.
- Keeps recovery data outside `.vfxphoto`, private Hot/Warm/Cold snapshots, Undo history, presets and document colour state; project format 15 and residency schema 16 remain unchanged.
- Retains serial queue execution, bounded memory/history, execution-time collision rechecks, atomic output publication and 0.11.0i.3 shutdown safety.
- Adds deterministic RGBA8/RGBA16 hidden-RGB round trips, disabled-draft isolation, explicit-resume collision confirmation, clean-preservation and malformed/wrong-typed quarantine coverage.

## 0.12.0e — Export Queue Foundation (2026-08-04)

- Moves Production Export execution into a modeless application-level queue while preserving the complete 0.12.0d plan and collision preflight.
- Captures immutable copy-on-write source image, layer tree, colour state, processing compatibility and resolved output/profile snapshots at enqueue time, preventing live-document or multi-document retargeting.
- Adds a bounded serial controller with stable job UUIDs and Pending, Running, Paused, Completed, Completed with issues, Failed and Cancelled states.
- Adds an Export Queue dock with progress, current-operation text, output counts, destination, detailed warnings/errors, Pause/Resume, Cancel Job, Cancel All, Remove and Clear Finished controls.
- Keeps the editor and document switching available while queued exports execute; shared native rendering remains serialised by the existing renderer locks.
- Adds cooperative pause checkpoints and cancellation through the shared render, tiled GPU/CPU resize and colour-conversion paths.
- Retains one-output-at-a-time memory use, atomic writes, execution-time collision rechecks and continuation after independent output failures.
- Releases captured pixel/layer snapshots at every terminal state, limits the queue to 16 unfinished jobs and retains at most 128 lightweight terminal/history records.
- Coordinates application close with queue cancellation and explicitly drains the controller before `MainWindow` child teardown.
- Adds deterministic queue-state, transition, progress-clamping, portable-identifier, snapshot-validation, pending-cancellation, unfinished-queue and retained-history-bound tests.
- Does not persist queue records across application restarts; recoverable descriptions and queue restoration remain 0.12.0f. Folder batch processing and automation remain 0.19.0.
- Does not change project format 15, private residency schema 16, colour-state schema 4, adjustment schema 10, vector schema 7, preset/export-profile formats or the 0.11.0i.3 shutdown contract.

## 0.12.0d — Production Multi-Output Export (2026-08-04)

- Adds a separate **File → Production Export…** workflow while retaining the existing approachable one-image quick exporter unchanged.
- Adds immutable production job descriptions with stable per-output identifiers, export-profile snapshots, filename templates, resize settings and collision policy.
- Renders the document once, derives each output surface in working RGB, then independently applies the selected colour/encoding profile and atomic file write.
- Supports original size, exact fit/stretch, long-edge and percentage output sizing with nearest, bilinear, bicubic, Lanczos 3 and area resampling.
- Prefers the native tiled GPU resampler for nearest/bilinear and uses the existing straight-RGBA CPU reference for higher-quality methods and fallback.
- Adds output add/duplicate/remove/reorder/enable controls, profile management, resolved-path preview and aggregate preflight in a font/screen-aware dialog.
- Adds ask-before-replace, overwrite, skip-existing and deterministic auto-rename collision policies, including execution-time collision rechecks.
- Isolates output failures and releases prepared images after each atomic write so completed files survive later failures without retaining every output surface.
- Adds cooperative progress/cancellation across the shared render and per-output work, and surfaces non-fatal ICC/OCIO embedding warnings against the completed file that produced them.
- Adds 13 deterministic tests for resizing, tokens, confirmed collisions, disabled drafts, duplicate paths, auto-rename, skip-existing, stable identifiers, invalid timestamps/resamplers, safe-surface limits and all-or-nothing preflight resolution.
- Does not add persistent queue scheduling/recovery, folder-wide batch processing or automation; those remain 0.12.0e–f and 0.19.0.
- Does not change project format 15, private residency schema 16, colour-state schema 4, adjustment schema 10, vector schema 7 or the 0.11.0i.3 shutdown lifecycle.

## 0.12.0c — Export Profiles and Naming Templates (2026-08-03)

- Adds version-1 `ExportProfileData` payloads inside the shared version-2 preset envelope, with stable built-in/user identities and atomic application-data storage.
- Stores format, bit depth, colour target, rendering intent, black-point compensation, ICC embedding, dither, Alpha/matte, quality and filename template in reusable profiles.
- Adds five deterministic built-in profiles for web PNG/JPEG, 16-bit PNG/TIFF masters and TGA Alpha interchange.
- Integrates export profiles into the existing single-image colour-managed export dialog, with the shared searchable manager and complete user CRUD/import/export operations.
- Adds portable filename tokens for document/profile/format/precision/dimensions/working/output spaces/date/time, deterministic per-dialog timestamps and literal brace escaping.
- Rejects unknown/unmatched tokens, path traversal, forbidden/control characters, reserved Windows names, trailing spaces/full stops and overlong resolved names before rendering.
- Allows profiles to change format and filename while retaining the initially chosen directory; writer availability and overwrite safety are rechecked after resolution.
- Adds explicit flatten-to-matte for Alpha-capable formats while preserving the existing hidden-RGB-safe Alpha path when preservation is selected.
- Adds deterministic export-profile, naming-template, CRUD/import-export and explicit-Alpha-flatten tests.
- Does not add multi-output export, resize, queue execution, folder batch processing or automation; those remain later milestones.
- Does not change project format 15, private residency schema 16, colour-state schema 4, adjustment schema 10, vector schema 7 or the 0.11.0i.3 shutdown lifecycle.

## 0.12.0b — Preset Management UX (2026-08-03)

- Adds one reusable `PresetManagerDialog` for adjustment and vector-appearance presets, with a font-aware initial/minimum size and a two-pane preset list/details layout.
- Adds live search across names, categories and tags; built-in/user source filtering; category filtering; favourites filtering; and recently-used filtering.
- Keeps ordinary adjustment application in the Inspector combo and vector application in the existing Appearance menu. A dedicated **Manage…** action exposes advanced organisation without cluttering those fast paths.
- Adds complete user-preset create, update-from-current, rename, duplicate, category/tag edit, favourite and delete controls for both supported preset kinds.
- Adds validated preset import/export for both supported kinds, including safe filename suggestions, remembered exchange directory and graceful kind/version/payload rejection.
- Makes built-in presets exportable, duplicable, favouritable and recent-aware while retaining immutable built-in definitions.
- Adds a bounded, atomic `usage.vfxpreset.json` sidecar keyed by stable preset ID for built-in favourite/recent state. It is application-global and never enters projects, snapshots, Undo or render identity.
- Shows preset source, category, tags, use count, last-used time, storage location and type-specific adjustment/vector summaries in the shared details pane.
- Preserves stable IDs through rename, metadata edits and update-from-current; duplicates receive new IDs and retain category/tags/favourite metadata.
- Adds deterministic tests for built-in usage separation, adjustment/vector management metadata, stable-ID retention, duplicate identity and validation failures.
- Does not change project format 15, private residency schema 16, colour-state schema 4, adjustment schema 10, vector schema 7, export pixels, GPU/CPU rendering or the 0.11.0i.3 shutdown lifecycle.

## 0.12.0a — Unified Preset Architecture (2026-08-03)

- Adds `PresetCore`, a shared version-2 `vfxphotolab-preset` envelope for adjustment, vector-appearance and future export-profile payloads.
- Adds stable deterministic built-in IDs, UUID-backed user IDs, versioned metadata, categories, tags, favourites, created/modified timestamps, recent-use timestamps and use counts.
- Adds platform-correct storage through `QStandardPaths::AppDataLocation/presets`, with subtype directories for adjustment kinds and bounded atomic `QSaveFile` writes.
- Preserves old adjustment-preset and vector-appearance files exactly while listing them. Explicit user changes migrate only the affected legacy file into the unified envelope after a successful atomic write.
- Extends adjustment and vector stores with create/save, rename, duplicate, update, delete, favourite, recent-use, import and export operations; imported/exported presets retain stable identity and validate feature kind and payload.
- Keeps built-in adjustment presets separate from user presets and gives each built-in a deterministic ID without placing built-ins in writable storage.
- Keeps embedded LUT adjustment payloads self-contained and unchanged, including generic LUTs, Tony McMapface and AgX.
- Records successful user-preset application as recent-use metadata while leaving built-ins read-only and free of user-state writes.
- Adds strict file-size limits, identifier validation, metadata limits, duplicate-ID/name isolation, managed-path deletion checks and graceful malformed-file warnings.
- Adds `VFXPhotoLabPresetTests` for deterministic envelope, validation, stable-ID, legacy-migration and import/export coverage.
- Does not change project format 15, private residency schema 16, colour-state schema 4, adjustment schema 10, vector schema 7, rendering, export pixels or the 0.11.0i.3 shutdown lifecycle.

## 0.11.0i.3 — Shutdown Heap-Corruption Blocker (2026-08-03)

- Fixes unbounded tool-options action retention. `QToolBar::clear()` removed each rebuilt generation from the visible toolbar but left the generated toolbar-owned `QWidgetAction` objects parented to the toolbar until shutdown. The options bar now removes those actions and defers their destruction safely after signal delivery; any pending generation is destroyed explicitly before `MainWindow` child teardown.
- Preserves externally owned actions defensively: only actions parented to the options toolbar are deleted; shared application actions are merely removed from the toolbar association.
- Adds explicit Smudge write-address validation for RGBA8, RGBA16 and greyscale targets before every direct scan-line mutation, including component-index and row-stride checks. Invalid addresses are skipped rather than touching memory.
- Replaces raw live-preview patch `memcpy` loops with one checked same-format rectangle copier that validates destination bounds, pixel stride, row sizes and scan-line availability before copying Smudge/effect-brush mask patches.
- Hardens sparse raster/channel history hashing and XOR application with exact image-size, byte-stride, tile-rectangle and payload checks; malformed deltas are rejected before any scan-line access.
- Adds a 200-generation toolbar ownership regression and corrupt sparse-history bounds/payload tests to the sanitizer-enabled test suite.
- Rejects ICC files smaller than the mandatory 128-byte ICC header before passing them to Qt, eliminating the `qt.gui.icc: fromIccProfile: failed size sanity 1` startup warning from malformed monitor/profile candidates.
- Adds first-class GCC/Clang AddressSanitizer and UndefinedBehaviorSanitizer support through the `sanitized` CMake preset, `run-sanitized.sh` and `scripts/test-sanitized.sh`. Normal `./run.sh` release builds are unchanged.
- Restores the pinned Linux and Windows wgpu-native 29.0.1.1 acquisition scripts referenced by the build and CI workflows, making the replacement archive self-contained instead of relying on an older overwrite target to retain them.
- Keeps project format 15, private residency schema 16, colour-state schema 4, adjustment schema 10 and vector schema 7 unchanged.

## 0.11.0i.2 — Smudge Stroke Engine Hardening (2026-08-03)

- Removes an incremental Smudge full-frame detach/copy caused by returning a shared working image and mutating its colour-space metadata on every pointer event. Incremental results now contain only status, affected bounds and diagnostics.
- Keeps dab spacing continuous across input segments. Long strokes no longer guarantee one expensive dab per raw mouse event; batch and incremental processing share the same distance accumulator and deterministic final tail.
- Skips preview publication and compositor work for high-frequency pointer events that do not yet cross the next dab-spacing threshold, preventing no-op events from building a UI backlog.
- Reuses the completed live Smudge surface directly when it already matches the target's full resolution and precision. Common photographs below the 4096-pixel preview ceiling therefore avoid replaying the whole stroke after release; larger downscaled previews retain deterministic replay fallback.
- Reduces transported-pixel sampling from two four-tap bilinear filters to one integer destination sample plus one fixed-offset bilinear source sample.
- Preserves whole-stroke/incremental equivalence, Finger Painting, selections, masks, component channels, 8/16-bit precision, straight Alpha and hidden RGB.
- Adds sparse history-delta builders that deduplicate only the tiles touched by path-local brush rectangles, avoiding scans of untouched tiles inside a long stroke's global bounding box. Matching format/colour-space history inputs remain immutable shallow references instead of being detached into another full-frame copy.
- Adds commit timing diagnostics reporting raw segment count, spaced dab count, commit path/time and history time.
- Extends deterministic coverage for no-image incremental results, event-count-independent dab spacing and sparse tile-history round trips.
- Retains the 0.11.0i.1 isolated GPU validation transfer and canvas-before-GPU shutdown ordering. No project, residency, colour-state, adjustment or vector schema changes.

## 0.11.0i.1 — Smudge Performance and Shutdown Stability (2026-08-02)

- Fixes progressively worsening Smudge latency by retaining one bounded immutable pre-dab scratch image for the stroke instead of allocating and destroying a cropped `QImage` for every transported dab.
- Replaces per-pixel `QRect::united()` dirty tracking with constant-time integer bounds and removes repeated scalar-neighbour reads and avoidable square roots in hard brush regions.
- Skips generic transformed selection-patch preparation for Clone/Heal and effect-brush previews that already apply their own selection contract.
- Normalises incremental Smudge working images to explicit `RGBA8888`, `RGBA64` or `Grayscale8` storage before direct scan-line writes, protecting blank layers and future callers from format/stride assumptions.
- Adds a deterministic long-stroke test covering whole/incremental equivalence, premultiplied-input normalisation and bounded scratch storage.
- Transfers the helper process's complete per-feature native GPU approval record to the GUI process. The normal application now creates its runtime device without rerunning display, managed-domain, adjustment, Fill, Gradient and hierarchy parity tests.
- Preserves every measured selective fallback from the helper, including exact CPU display/managed-domain fallback and deep mixed-hierarchy compositor fallback.
- Corrects application shutdown ordering: the helper is stopped, global workers are drained, `MainWindow`/`ImageCanvas` and their `QImage` paint engines are destroyed, and only then are native WebGPU resources released.
- Keeps project format 15, private residency schema 16, colour-state schema 4, adjustment schema 10 and vector schema 7 unchanged.

## 0.11.0i — Persistence, Residency and Final Hardening (2026-08-02)

- Completes the 0.11.0 Colour Management Foundation milestone without changing project, snapshot, adjustment, vector or colour-state schema versions.
- Adds an immutable colour-resource audit for external ICC files and saved OpenColorIO configurations. Missing, unreadable, oversized or fingerprint-changed resources are reported without relinking or substituting another profile/configuration.
- Keeps embedded ICC bytes authoritative when their original source file disappears or changes. A missing source file becomes a runtime warning rather than an automatic project repair or pixel reinterpretation.
- Separates project-data repairs from colour-resource availability in the open workflow. Only repaired layer/mask/selection data marks the project modified; resource warnings are consolidated into the same one-time report without creating Undo history.
- Re-runs the resource audit after private Cold restoration and after project save, while keeping runtime warnings out of persisted project/snapshot data.
- Tightens external ICC descriptor validation to exact SHA-256 fingerprint length, requires embedded ICC bytes to match that fingerprint, bounds monitor-profile reads to 16 MiB, and rejects malformed embedded source-image base64 instead of accepting partial decoding.
- Makes presentation metadata replacement idempotent, matching the existing output-settings contract: submitting unchanged settings leaves the dirty flag, processing revision and image/cache identities untouched.
- Adds deterministic tests for missing and changed ICC resources, unavailable OCIO configurations, no-substitution behaviour, Cold-residency re-audit, embedded-profile authority and metadata-only idempotence.
- Expands final conformance documentation and regression coverage across legacy projects, multi-document isolation, display/proof GPU fallback, managed adjustments, colour-managed export, bit depth, Alpha/hidden RGB and cancellation.
- Marks 0.11.0 complete; the next planned milestone is 0.12.0 Presets and Production Export Foundation.

## 0.11.0h — Colour-Managed Export, Bit Depth and Blue-Noise Dithering (2026-08-02)

- Replaces the implicit flattened-image save with an explicit full-resolution working-to-output export contract. The authoritative hidden-RGB composite is promoted to straight RGBA64 before any ICC or OCIO conversion.
- Adds an export settings dialog with saved document output profile, rendering intent, black-point-compensation request, ICC-embedding preference, format-aware bit depth, quality, transparency matte and deterministic blue-noise controls.
- Supports ICC→ICC, ICC→OCIO, OCIO→ICC and OCIO→OCIO output conversion through the existing ordinary colour-space transform service. OCIO Display/View, monitor ICC, soft proofing and gamut warning remain presentation-only and are never sampled by export.
- Makes format capabilities explicit: PNG/TIFF expose 8- and 16-bit integer channels; JPEG/TGA/WebP/BMP remain 8-bit; PNG/JPEG/TIFF can carry ICC metadata in this path; OCIO and unsupported metadata formats are reported as untagged.
- Flattens formats without Alpha against a user-selected matte after output-space conversion. Alpha-capable formats preserve straight Alpha and hidden RGB.
- Adds a checked-in deterministic 64 × 64 blue-noise rank tile for RGB-only 16→8 quantisation. Neutral channels share one threshold, exact black/white endpoints are retained and Alpha is never dithered.
- Persists output defaults in project and Hot/Warm/Cold colour state without creating Undo entries, changing the processing revision or invalidating compositor, histogram or presentation caches.
- Adds deterministic tests for format contracts, hidden-RGB and Alpha preservation, display/proof isolation, untagged warnings, matte flattening, cancellation, 16-bit PNG precision/profile round-trip and output-default persistence.
- Does not begin the 0.11.0i final persistence/residency/conformance hardening pass.

## 0.11.0g — GPU/WGSL Colour Transform Integration (2026-08-02)

- Adds a dedicated WGSL presentation kernel for ICC monitor transforms, OCIO Display/View processing, soft-proof simulation and the deterministic gamut-warning overlay.
- Keeps the CPU colour-management chain authoritative by baking each complete presentation transform into a deterministic 65³ RGBA16Float lattice. GPU execution evaluates that reference lattice with manual trilinear interpolation rather than reimplementing ICC or OCIO semantics in shader code.
- Supports both RGBA8 and RGBA16 presentation surfaces. The 16-bit path uploads and reads RGBA16Float while restoring source Alpha exactly; all paths preserve presentation-only separation from document pixels and hidden working RGB.
- Adds matching transform-fingerprint CPU-lattice and GPU-texture caches with bounded 48 MiB LRU budgets. Identical monitor/proof/view states are reused across documents and thumbnails; ordinary transforms share one lattice texture; the second proof round-trip lattice is allocated only while gamut warning is active.
- Adds deterministic per-transform CPU probe validation before caching, then extends native startup validation with real ICC CPU/GPU parity for straight/premultiplied 8-bit and 16-bit surfaces plus a deterministic synthetic gamut-warning probe. Failure disables only display-transform GPU execution and retains the exact CPU fallback.
- Routes full canvas images, progressive tiles, live brush previews, transform previews and derived document-strip thumbnails through the validated GPU path without changing Undo, project schemas, residency state, merge/copy behaviour or exports.
- Adds deterministic lattice-generation/reference tests and reports the active presentation backend in Display Colour Management and Document Colour Information.
- Adds paired working↔adjustment-domain RGBA16Float lattices for managed 8-bit compositor stacks. Linear-working Exposure and encoded-sRGB/Rec.709 colour/luminance adjustments now remain on the tiled WGSL compositor when both the per-transform probe and startup end-to-end parity gates pass.
- Extends the adjustment shader without changing operator contracts: domain conversion and 8-bit quantisation occur before the existing operator, the inverse conversion occurs before working-space blending, and Alpha/hidden RGB remain untouched.
- Adds separate bounded CPU/GPU LRU caches for managed domain pairs and deterministic Display-P3 managed Exposure/Saturation tests. Unsupported profiles, lattice failures or driver mismatches fall back only the affected managed stack to `ImageProcessor`.
- Does not begin 0.11.0h colour-managed export, bit-depth reduction or blue-noise dithering.

## 0.11.0f — Display Colour Management and Soft-Proof Foundation (2026-08-02)

- Adds a presentation-only CPU colour pipeline from the document working space through optional proof simulation to either an ICC monitor profile or an OpenColorIO Display/View processor.
- Adds Fedora/Linux monitor-profile discovery through `colormgr`/colord, Windows discovery through Windows Color System, an application-wide manual ICC override, an environment override and safe sRGB fallback.
- Refreshes the active monitor transform when the application window changes screens and exposes the detected screen, profile, source, fallback and transform status in the colour-management UI and document status.
- Separates OCIO `DisplayViewTransform` processing from ordinary OCIO colour-space conversion and preserves configuration fingerprints, missing-config protection and explicit relinking.
- Adds document-level soft-proof enablement, ICC proof-profile selection, rendering-intent and black-point-compensation requests, plus deterministic round-trip gamut-warning groundwork.
- Keeps authoritative canvas pixels, layers, semantic colours, masks, selections, history, Merge Layers, Copy Merged, raw thumbnail caches and export rendering independent from display transforms. Display/proof edits are persisted metadata and create no Undo command.
- Adds parallel presentation copies for the canvas backing image, progressive tiles, live paint surface, transform previews and document-strip thumbnails, with transform-fingerprint invalidation when monitor, display/view or proof settings change.
- Advances the internal colour-state JSON schema to 4. Existing schema-1/2/3 projects migrate with colour-managed presentation disabled, preserving their previous appearance until explicitly enabled; new managed documents use automatic monitor ICC presentation.
- Extends project and Hot/Warm/Cold tests with display/proof persistence and adds deterministic display, Alpha-preservation, gamut-warning and fallback coverage.
- Does not begin the 0.11.0g GPU/WGSL colour-transform implementation; the authoritative 0.11.0f path remains CPU-based.

## 0.11.0e.3 — Colour-management dialog sizing polish (2026-08-02)

- Gives Colour Management Preferences, OpenColorIO / ACES Settings and Document Colour Information font-aware initial and minimum sizes instead of opening with a zero-height resize request.
- Prevents combo boxes, explanatory text and buttons from appearing vertically compressed under Fedora KDE until the user manually resizes the window.
- Retains the OpenColorIO 2.5.2 GCC 16 bootstrap and static-package handoff fixes from 0.11.0e.1/e.2.
- Prunes disposable OCIO source and object intermediates after a successful install while retaining the installed package and static dependency prefix required for incremental application links.
- No document pixels, colour transforms, OCIO configuration state, LUT behaviour, persistence or GPU rendering changed.

## 0.11.0e.2 — OpenColorIO package handoff fix (2026-08-02)

- Passes the installed OpenColorIO package directory and its private static dependency prefix into the main VFX Photo Lab configure.
- Fixes the case where OpenColorIO built successfully but VFX Photo Lab still configured in ICC-only mode.
- Prints the exact package and dependency paths selected by `run.sh`.
- Suppresses a GCC 16 array-bounds false-positive only inside the vendored OpenColorIO build.

## 0.11.0e.1 — OpenColorIO and ACES Foundation build fix (2026-08-02)

- Fixed the vendored yaml-cpp 0.8.0 build with GCC 16 by providing the required standard `<cstdint>` declarations during the private OCIO dependency build.
- Recreates an incomplete OCIO build tree so a previous failed dependency configure cannot retain stale compiler flags.
- Removed unused-function warnings from explicit ICC-only fallback builds.
- No document, colour-conversion, LUT, GPU, persistence or UI behaviour changed.

## 0.11.0e — OpenColorIO and ACES Foundation (2026-08-02)

- Adds optional OpenColorIO 2.5.2 integration with checksum-pinned local acquisition and an ICC-only build fallback.
- Adds document-level selection of the built-in ACES 2 CG and Studio configs, `$OCIO`, and external `.ocio`/`.ocioz` configurations.
- Persists configuration source, immutable identifier, version, SHA-256 fingerprint, ICC bridge space, selected OCIO working spaces and display/view metadata in colour-state schema 3; schema 1/2 states migrate with OCIO disabled.
- Exposes eligible scene-referred OCIO spaces—including ACEScg and ACES2065-1—in Assign Profile and Convert to Profile.
- Uses OpenColorIO CPU processors as the correctness reference and bridges ICC working profiles through the built-in `sRGB - Texture` interchange space while preserving Alpha and hidden RGB.
- Validates display/view selections for the upcoming display-management stage without applying them to document pixels or the current canvas.
- Detects missing or changed configs and requires explicit relinking; no similarly named config or colour space is silently substituted.
- Preserves all Legacy V1 rendering, ICC-only documents and specialised generic/Tony McMapface/AgX LUT contracts.
- Adds deterministic colour-state migration, built-in ACES inspection, CPU round-trip, Alpha-preservation and changed-fingerprint tests.

## 0.11.0d — Colour-Correct CPU Processing Contracts (2026-08-02)

- Adds explicit adjustment-domain classification for encoded working, linear working, encoded sRGB/Rec.709, raw-component and LUT-contract operations.
- Routes managed Exposure through a same-primaries linear working space and routes Rec.709/sRGB-defined colour/luminance operators through an explicit sRGB domain before returning to the document working space.
- Reuses the central colour-transform cache for adjustment-domain transforms while preserving straight Alpha and hidden RGB.
- Preserves exact Legacy V1 adjustment behaviour for pre-0.11 projects and explicit legacy documents.
- Carries processing compatibility through previews, histograms, exports, merged copies, raster merging, trimming and Hot/Warm/Cold-aware render requests.
- Keeps native GPU compositing enabled for unaffected managed stacks; only domain-sensitive managed adjustment stacks temporarily use the CPU reference path pending 0.11.0g.
- Adds deterministic domain, legacy-protection, wide-gamut, Alpha and 8/16-bit consistency tests.

## 0.11.0c — Working Spaces, Assign Profile and Convert to Profile (2026-08-02)

- Added sRGB, Linear sRGB, Display P3, Adobe RGB and ProPhoto RGB working-space choices for new documents, plus supported external `.icc`/`.icm` profile selection for existing documents.
- Added **Image → Colour Management → Assign Profile**. Assign changes interpretation metadata and raster profile tags without changing RGB component values, records one Undo step and preserves original input-origin metadata.
- Added **Convert to Profile** with one cached ICC transform, transactional preparation, progress/cancellation, stale-result rejection and one-step Undo/Redo. Conversion covers the canvas, raster layers, vector fills/strokes, text colours and Gradient Map stops while leaving Alpha, masks and selections unchanged.
- Preserved hidden RGB by converting straight RGBA8/RGBA16 payloads independently of Alpha. Added deterministic 8-bit, 16-bit, semantic-colour, mask, cancellation and external-profile tests.
- Extended structural history snapshots to include semantic document colour state. Undo/Redo receives a fresh colour-state cache revision, and retained ICC bytes are included in history memory accounting.
- Public project and Hot/Warm/Cold restore now explicitly retag every raster payload with the persisted working profile. Version-15 projects and version-16 residency snapshots remain unchanged and backward compatible.
- Existing documents remain Legacy V1 until the user explicitly assigns or converts a profile. General adjustment-domain correctness and application-controlled display management remain deferred to 0.11.0d and 0.11.0f respectively.

## 0.11.0b — ICC Import and Untagged Policy (2026-08-02)

- Added guarded ICC inspection for image open, preserving valid embedded profile bytes and stable content fingerprints where available while keeping decoded pixels unchanged.
- Distinguishes valid embedded profiles, genuinely untagged images and damaged/unsupported embedded profile declarations. Invalid declarations no longer block image opening and use the explicit untagged policy.
- Added an application-wide untagged-image policy with **Assume sRGB**, **Ask every time** and **Leave untagged** choices. The same policy now covers Open Image, drag-and-drop, New from Clipboard and external image Paste.
- Added **Edit → Colour Management Preferences** and **Image → Document Colour Information** with clear import status, interpretation, working-space, fingerprint and compatibility diagnostics.
- Advanced the colour-state JSON schema to 2 while retaining `.vfxphoto` version 15 and Hot/Warm/Cold schema 16. Schema-1 colour states migrate without pixel conversion or appearance changes.
- Added deterministic tests for policy assignment without pixel mutation, explicit untagged clipboard materialisation, embedded-profile project persistence, invalid-profile fallback and schema-1 migration.
- No Assign/Convert workflow, file-open/new-document working-space conversion, display management, OCIO processing or adjustment-math change is active yet. Existing cross-profile clipboard Paste conversion remains in place.

## 0.11.0a.2 — Paint Commit Colour-Identity Fix (2026-08-02)

- Fixed Brush/Eraser and related asynchronous raster, mask and channel strokes vanishing on mouse release in newly managed documents. The live backend identity now carries the document colour-state revision through the entire paint commit instead of defaulting to revision zero and being rejected as obsolete.
- Paint completion now validates the same full session identity before publishing history or pixels.
- `RenderSessionContext` live construction now requires all three identity fields, preventing future two-field call sites from silently omitting colour-state identity.
- No colour transform, adjustment, LUT, persistence or project-format behaviour changed.

## 0.11.0a.1 — Warning-Free Colour-State Foundation Build (2026-08-02)

- Replaced deprecated `QCryptographicHash::addData(const char *, qsizetype)` calls with the Qt 6 `QByteArrayView` overload.
- Changed colour-transform cache entry statistics to use `qsizetype`, matching `QHash::size()` and eliminating the GCC narrowing warning.
- No colour-state, persistence, rendering, cache, project-format, residency, ICC/LUT or GPU behaviour changed from 0.11.0a.

## 0.11.0a — Colour-State Architecture and Legacy Protection (2026-08-02)

- Added explicit document colour state separating input profile, working space, display transform, proofing state and default output profile without applying any new transform in this release.
- Added stable semantic descriptors and fingerprints for untagged, built-in, embedded ICC, external ICC and future OCIO colour spaces, plus a thread-safe cached Qt colour-transform service.
- Advanced public `.vfxphoto` projects to version 15. Versions 1–14 migrate to explicit Legacy V1 processing without pixel conversion, retagging, display-transform activation or adjustment/LUT reinterpretation.
- Advanced private Hot/Warm/Cold snapshots to version 16 and persist the complete colour state; older snapshots restore into Legacy V1.
- Added colour-state revision identity to composite tiles, progressive preview requests, histograms, layer thumbnails and document-strip thumbnails so future profile changes cannot publish stale cached results.
- Added deterministic state/persistence tests, exact version-14 migration render coverage, transform-cache coverage, cold-residency round trips and tile/histogram cache invalidation tests.
- Adjustment schema remains 10, vector schema remains 7 and all existing CPU/WGSL, LUT, alpha-safe hidden-RGB, selection, group, clipboard, text/vector/SVG, Fill/Gradient and Merge Layers behaviour is intentionally unchanged.

## 0.10.1g.1 — UI, Persistence, Presets and Hardening Build Fix (2026-08-02)

- Fixed the Fedora/GCC build failure caused by an ambiguous `lutDomainSourceDisplayName()` call in the LUT Inspector helpers. The Inspector-specific formatter now has a distinct internal name, while the shared adjustment display-name API remains unchanged.
- No LUT processing, persistence, preset, project-format, GPU-path or schema behaviour changed from 0.10.1g.

## 0.10.1g — UI, Persistence, Presets and Hardening (2026-08-02)

- Expanded the LUT Inspector with table dimensions, editable Generic trilinear/tetrahedral interpolation, operator and processing contracts, exact 1D/3D domains and directive origins, deterministic table fingerprint and selectable diagnostics.
- Added actionable evaluation status for exact 16-bit CPU rendering, payload/domain transport fallback, unavailable native WebGPU and failed startup LUT parity; approved eight-bit LUTs identify the RGBA16Float native path.
- Added **Copy Details** for a complete embedded LUT, document-space and evaluation report.
- Centralised table/domain GPU fallback explanations in `CubeLut` so UI, texture construction and tests agree.
- Added duplication and Hot/Warm/Cold snapshot regressions proving embedded tables, domains, interpolation, processing and Strength survive residency round-trips with identical RGBA64 output.
- Hardened adjustment presets with a 32 MiB pre-read/write limit and explicit replacement confirmation instead of silent storage-name overwrite.
- Added warning/success Inspector styling and expanded project/preset/fallback integration coverage. Adjustment schema remains 10, project format remains 14 and residency schema remains 15.

## 0.10.1f — Floating-Point GPU LUT Path (2026-08-02)

- Replaced the imported-LUT `RGBA8888`/`QImage` transport with direct `RGBA16Float` table packing and native texture upload. Fractional, negative and greater-than-one table samples are no longer reduced to eight-bit lookup values before evaluation.
- Added WGSL implementations of deterministic trilinear and tetrahedral 3D interpolation, including the authoritative CPU tie precedence, plus linear 1D shaper sampling and per-channel 1D/3D domains.
- Added native transfer-aware evaluation for Encoded document values, Linear sRGB / Rec.709 and Raw component processing contracts. Strength continues to blend after returning to document component space.
- Added native Tony McMapface and AgX Base sRGB pipelines with the same preprocessing, tetrahedral table sampling and output operations as the CPU reference.
- Expanded startup CPU/GPU conformance to independent tetrahedral, trilinear, linear-processing, Tony and AgX cases. The LUT feature bit is enabled only when every LUT mode passes; failure disables LUT acceleration without removing approved non-LUT adjustment, compositor, Fill, Gradient, brush or Clone Stamp capabilities.
- LUT payloads outside finite binary16 table range, domains outside finite ordered f32 range, oversized lookup textures or adapters unable to create the floating-point texture use the authoritative CPU fallback. 16-bit documents continue using the exact RGBA64 CPU path.
- Made `shaders/adjustment_tile.wgsl` the single runtime source by embedding it through a generated CMake header, removing the previous duplicate adjustment shader string.
- Added floating-point packing, precision/range, extended-value, mode eligibility and named-operator coverage. No persisted setting changed: adjustment schema remains 10, project format remains 14 and residency schema remains 15.

## 0.10.1e — Specialised LUT Operator Pipelines (2026-08-02)

- Added persisted **Generic .cube**, **Tony McMapface** and **AgX Base sRGB** operator profiles. Common reference filenames and titles are detected during a fresh LUT import, and the Inspector provides an explicit manual selector.
- Tony McMapface now evaluates the documented linear-Rec.709 `x / (x + 1)` preprocessing stage, tetrahedral table sample and linear-output return to the document transfer function.
- AgX Base sRGB now evaluates the supplied linear Rec.709 to FilmLight E-Gamut matrix, `[-12.47393, 12.5260688117]` log2 exposure allocation, tetrahedral table sample and power-2.4 output stage before returning to the document transfer function.
- Named operator profiles force their required tetrahedral interpolation and use selective authoritative CPU fallback. Generic LUTs retain all existing transfer-aware tiled GPU eligibility, so unrelated adjustments and compositor features remain accelerated.
- Added clear warnings for missing 3D data, unsupported document primaries and non-reference cube sizes. The generic processing selector is disabled while a named profile is active because the named profile fixes its surrounding transform.
- Advanced adjustment schema to 10. Schemas 1–9 always migrate to **Generic .cube** rather than being reinterpreted by their saved source name; public project format remains 14 and private residency schema remains 15.
- Added independent reference-math, auto-detection, JSON migration, project save/reopen, Strength, CPU-fallback, RGBA8/RGBA64, Alpha and hidden-RGB regression coverage for both named pipelines.

## 0.10.1d — LUT Input and Output Semantics (2026-08-02)

- Added persisted LUT processing contracts for **Encoded document values**, **Linear sRGB / Rec.709** and **Raw component values**. New imports use the encoded compatibility contract.
- The authoritative CPU evaluator now performs extended-range sRGB transfer conversion around the 1D/3D table when the selected contract differs from the document's sRGB or linear-sRGB transfer state. Strength blends only after the result returns to the document component space.
- Added explicit RGBA8/RGBA64 destination clamping while preserving negative and greater-than-one scalar intermediates through table evaluation and transfer conversion.
- Added document-profile-aware selective GPU eligibility. The existing RGBA8 WGSL path remains enabled when the chosen contract reduces to direct component sampling; transfer conversion, tetrahedral 3D LUTs and extended-range tables fall back to the exact CPU path without disabling unrelated GPU features.
- Added an Inspector Processing selector, persisted summaries and an explicit warning for valid ICC profiles outside the current sRGB/linear-sRGB contract. Such profiles are preserved and sampled without a hidden gamut conversion until 0.11.0.
- Advanced adjustment schema to 9. Schemas 1–8 migrate to Encoded document values; schemas 1–7 continue migrating to trilinear interpolation. Public project format remains 14 and private residency schema remains 15.
- Added deterministic scalar, transfer, Strength-space, GPU-eligibility, 8/16-bit, hidden-RGB, project and preset regressions for all three contracts.

## 0.10.1c — Authoritative CPU LUT Evaluator (2026-08-02)

- Replaced implicit LUT evaluation with an explicit scalar pipeline: per-channel 1D-domain clipping, optional shaper sampling, per-channel 3D-domain clipping, red-fastest lattice lookup and Strength blending.
- Added deterministic trilinear and tetrahedral interpolation with fixed branch precedence on exact tetrahedron ties.
- Removed the evaluator's final `[0,1]` clamp. Extended outputs survive until the integer destination boundary.
- New 3D and combined imports default to tetrahedral CPU evaluation. Existing adjustment schemas 1–7 migrate to trilinear to preserve their historical appearance.
- Advanced adjustment schema to 8 for interpolation persistence while keeping public project format 14 and private residency schema 15.
- Added interpolation probes, tie cases, unclamped-output checks, migration coverage and RGBA8/RGBA64 tetrahedral consistency tests. The legacy RGBA8 GPU table remains trilinear until 0.10.1f.

## 0.10.1b — LUT Parser and Data-Model Conformance (2026-08-02)

- Replaced the permissive `.cube` reader with an explicit header/shaper/3D-table state machine. Valid combined 1D shaper plus 3D files consume their declared row counts in order, while directives appearing after data, missing rows and extra rows now fail with actionable line-numbered errors.
- Added strict duplicate and conflict detection for `TITLE`, size declarations, `DOMAIN_MIN`/`DOMAIN_MAX` and LUT-specific input ranges. Ambiguous mixtures of generic domains and table-specific ranges are rejected instead of silently choosing one interpretation.
- Recognises `LUT_IN_VIDEO_RANGE` and `LUT_OUT_VIDEO_RANGE` but rejects them clearly until their required range conversion exists. Unknown semantic directives are likewise rejected rather than being silently ignored.
- Removed the former `[-16,16]` parser/data-model clipping. All finite table values representable by the existing float payload and all finite positive-span domains are retained; extended-range LUTs continue to use the existing exact CPU fallback because the native lookup texture remains RGBA8 in this stage.
- Added persisted domain-origin metadata for default ranges, `DOMAIN_*`, LUT-specific input ranges and migrated legacy payloads. Adjustment schema is now 7 with schema 1–6 migration; public `.vfxphoto` format remains 14.
- Added asymmetric red-fastest lattice coverage, wide-domain and table-specific combined fixtures, malformed/ambiguous fixture coverage, parser diagnostic checks and persistence/migration regression tests. LUT interpolation, encoded/linear processing, final output clamping and the RGBA8 GPU lookup remain intentionally unchanged for later 0.10.1 stages.
- The New from Clipboard workflow introduced in 0.10.1a remains unchanged.

## 0.10.1a — LUT Conformance Fixtures and Baseline (2026-08-02)

- Added a deterministic `.cube` fixture corpus covering 1D/3D identity, red/blue swap, inversion, channel isolation, range clipping/remapping, stepped tables, non-default domains, combined 1D shaper plus 3D tables, extended output, wide finite values and fractional GPU precision.
- Added an independent test-only Cube parser and scalar trilinear evaluator with authoritative input/output vectors. It does not share production indexing or evaluation code and explicitly validates red-fastest lattice order.
- Pinned the current implementation’s known correctness defects without presenting them as success: final `[0,1]` output clamping, finite table clipping to `[-16,16]`, and `RGBA8888` GPU LUT quantisation.
- Added fixture-driven RGBA8/RGBA16 CPU consistency, hidden-RGB/Alpha preservation, adjustment JSON, `.vfxphoto` save/reopen, flattened-render equivalence and LUT preset persistence baselines. No production LUT parsing/evaluation behaviour is changed in this stage.
- Added **File → New from Clipboard** and a **From Clipboard** welcome action. New documents use the copied pixel size and base raster, preserve 8/16-bit precision, resolution and embedded colour space where available, assign sRGB to untagged images, and enforce the normal 32,768-pixel document limit.
- Project format remains 14, adjustment schema remains 6, vector schema remains 7 and Hot/Warm/Cold residency schema remains 15.

## 0.10.0l — Integration and Hardening (2026-08-01)

- Added bounded project-layer preflight before recursive decoding: layer trees are limited to 128 levels and 8,192 nodes, invalid group-child payloads are rejected, and duplicate/invalid IDs are checked only after the hierarchy is known safe to traverse.
- Added cumulative vector-complexity limits at layer and project scope. Vector layers now reject more than 1,000,000 editable path nodes, while projects reject excessive aggregate object/node payloads before allocating or normalising them. Existing per-object limits remain in force.
- Hardened embedded raster decoding with PNG-only validation and a 32,768-pixel dimension ceiling for source, layer and mask payloads. Unsupported raster encodings, non-finite opacity/legacy adjustment values and excessive guide arrays are rejected or safely recovered according to whether the payload is authoritative or optional.
- Applied the same hierarchy/vector/storage validation to save, structural replacement and layer-tree replacement paths so malformed or excessive data cannot enter history, document residency snapshots or public project files through an internal operation.
- Added an integrated RGBA64 round-trip regression combining selection-aware Fill, Diamond Gradient, a feathered selection, guides, opacity/blending/masks, raster merge, semantic vector-to-path merge and exact save/reopen rendering. Added direct malformed hierarchy, guide and raster-encoding regression cases.
- Consolidated the Fedora integration plan for clipboard/duplication, SVG, Image Size, Canvas Size, masks/channels/groups, Hot/Warm/Cold eviction, Undo/Redo, 8/16-bit documents, large-image performance, hidden RGB, Fill/Gradient GPU/CPU consistency and both merge types.
- No renderer, project-format or private-residency schema bump: project format remains 14, vector schema remains 7, vector appearance schema remains 2 and Hot/Warm/Cold residency schema remains 15.

## 0.10.0k.1 — Merge Layers Selection and Vector Editability Fix (2026-08-01)

- Fixed layer-panel context clicks so right-clicking any member of an existing ExtendedSelection preserves the complete selection while moving current-row focus. **Merge Selected Layers** can now be invoked directly from the context menu without silently reducing the operation to one layer.
- Added explicit active-object identity for multi-object vector layers. Direct Selection and Corner hit-testing now scans every rendered object, switches the editable path by object UUID and keeps node, hover, marquee and compound-contour state isolated to that object.
- Extended Pen continuation, endpoint hover and path joining to distinguish objects within the same merged vector layer. Same-layer joins now replace the active object and remove only the donor object atomically instead of assuming one object per layer.
- Removed remaining first-object assumptions from node dragging, insertion, deletion, corner operations, close/open commands and path conversion. Deleting one path object from a merged layer retains all other objects and selects safely.
- Layer Inspector Fill, Stroke, opacity, width, alignment, caps, joins, miter, dash and arrowhead edits now apply across every object in a merged vector layer as one Undo operation; geometry controls continue to edit only the currently active object.
- Added a LayerTreeWidget regression test for context-click multi-selection preservation. Project format remains 14, vector schema remains 7, vector appearance schema remains 2 and Hot/Warm/Cold residency schema remains 15.

## 0.10.0k — Merge Layers (2026-08-01)

- Added **Merge Selected Layers** to the Layer menu and layer context menu. The command accepts at least two contiguous sibling layer roots and rejects cross-group, non-contiguous and mixed raster/vector selections so unrelated stacking and coordinate systems cannot change silently.
- Raster layers merge only with raster/legacy Base Image layers. The selected stack is evaluated in isolation with transforms, masks, layer opacity, blend modes, straight RGBA and hidden RGB applied, then stored as one compact raster-reference payload. Revealable pixels outside the current canvas are retained, with bounded transformed-storage preflight before allocation. The top selected layer keeps its identity and name; the result is visible, 100% opacity, Normal/Copy, mask-free and positioned correctly inside transformed parent groups.
- Vector layers merge only with vector layers. Every semantic rectangle, rounded rectangle, ellipse, line, polygon, star and arrow is converted to an ordinary editable Bézier path. Affine object/layer transforms are baked into parent-local path nodes, compound paths and fill rules are retained, object order is remapped from layer stacking to vector draw order, and fresh object/node IDs prevent selection collisions.
- Vector merge preflight currently requires visible, unmasked, 100%-opacity Normal/Copy layers and rejects projective transforms; these restrictions preserve editable vector appearance instead of silently rasterising unsupported layer-level effects.
- Added detached preflight and one validated layer-tree replacement transaction, atomic Undo/Redo selection restoration, renderer/cache invalidation, and core regression coverage for raster hidden-RGB compositing and vector visible-equivalence after shape conversion.
- Project format remains 14, vector schema remains 7, vector appearance schema remains 2 and Hot/Warm/Cold residency schema remains 15.

## 0.10.0j.1 — Gradient Live Preview Performance Fix (2026-08-01)

- Reworked live Gradient placement so pointer movement no longer performs full-preview gradient generation and whole-document compositing synchronously on the UI thread.
- Added a 16 ms coalescing timer and a dedicated immutable preview worker. The placement line and handles remain immediate, while pending pointer updates collapse to the latest geometry and old-gesture frames are discarded and generation serials prevent out-of-order publication.
- Added a screen-resolution working tier bounded to the current canvas demand (960–1600 pixels on the longest side). The committed release path remains full resolution and continues to use the existing parity-gated tiled GPU application or exact CPU fallback.
- Added single-submit interactive compositing for live raster previews, asynchronous CPU/channel/mask fallback, transformed mask-preview publication and cancellation tokens so release, Escape, tool changes and document/session changes cannot publish obsolete frames.
- Added reduced live-composite support in `ImageCanvas`. A transient lower-resolution surface is scaled only for display and is never promoted into the authoritative preview backing; ordinary same-size paint previews retain the existing rapid-following-stroke promotion behaviour.
- Added canvas regression coverage proving reduced transient composites display correctly but cannot seed later strokes. Project format 14, vector schema 7, vector appearance schema 2 and residency schema 15 remain unchanged.

## 0.10.0j — Gradient Tool (2026-08-01)

- Implemented the raster **Gradient Tool** and stacked it with Fill in one right-click toolbar family. Gradient supports **Linear**, **Radial**, **Angle/conical**, **Reflected** and **Diamond** geometry, persistent type/colour/reversal options and a fixed-screen document-space placement line with distinct start/end handles.
- Added **Primary to Secondary** and **Foreground to Transparent** colour modes. Raster colour Alpha is interpolated normally; masks and editable scalar channels interpret Transparent as a zero-valued endpoint. Reversal changes gradient direction without mutating the current colours.
- Added non-destructive live preview while dragging. Preview geometry is mapped through raster/mask reference transforms, scaled through the existing large-document preview tier and rendered through the current channel/mask/compositor presentation. Release computes the authoritative full-resolution result; Escape or a sub-pixel click cancels without history.
- Active hard or feathered selections constrain application coverage. Raster feathering blends in premultiplied space while storing straight RGBA and retaining meaningful hidden RGB whenever output Alpha is zero. Masks, greyscale and individual Red/Green/Blue/Alpha targets use scalar coverage. RGB8, RGB16 and grayscale documents share the same target semantics.
- Added 256×256 tiled application. Eligible 8-bit raster, mask and editable-channel tiles use `gradient_apply.wgsl`; a feature-specific startup diagnostic compares all five modes and raster/channel/mask targets against `applyGradientCpu()`. Any disagreement or native failure disables/falls back only Gradient, discards provisional native tiles and reruns the complete operation through the exact CPU reference. RGBA64 remains exact CPU work.
- Successful release commits one target-specific XOR tile-delta Undo command and one session/surface invalidation. Added core coverage for every geometry mode, reversal, transparent endpoints, masks, channels, feathering, hidden RGB, 16-bit output and exact cross-tile CPU parity.
- Public project format remains 14, vector schema remains 7, vector appearance schema remains 2 and private residency snapshots remain schema 15.

## 0.10.0i.1 — Fill GPU Shader Validation Fix (2026-08-01)

- Fixed the native Fill WGSL parser failure caused by assigning directly to the `.rgb` multi-component swizzle. The shader now constructs and assigns complete `vec4<f32>` values while preserving the same alpha-safe CPU/GPU arithmetic.
- Added build-time validation for unsupported multi-component WGSL swizzle assignments across standalone shaders and embedded C++ shader blocks.
- Added a matching runtime guard before shader-module creation, preventing this known-invalid source pattern from reaching pipeline creation or queue submission even when the project is built outside the supplied scripts.
- Kept project format 14, vector schema 7, vector appearance schema 2 and Hot/Warm/Cold residency schema 15 unchanged.

## 0.10.0i — Fill Tool (2026-08-01)

- Implemented the raster bucket **Fill Tool** for existing raster/Background pixels, layer masks, the greyscale view and individual Red, Green, Blue and Alpha edit targets. Fill uses the primary or secondary colour, including colour Alpha where raster transparency is not preserved.
- Added 0–255 straight-component tolerance plus **Contiguous** connected-region and global replacement modes. Fully transparent raster pixels compare by apparent transparency so unrelated hidden RGB does not split an apparently empty region.
- Active selections constrain both region traversal and final application coverage. Feathered raster coverage blends in premultiplied space to avoid transparent-edge contamination, while channel and mask coverage remains scalar; clicks outside the selected area or editable target make no document change.
- Added **Preserve Transparency** for raster-pixel fills. Stored pixels remain straight RGBA, partial raster coverage uses premultiplied blending, meaningful source RGB survives whenever output Alpha becomes zero, editable channels change only the requested component, masks remain compact when uniformly filled, and 8/16-bit CPU behaviour is consistent.
- Region discovery uses one deterministic CPU reference. Application is split into bounded 256×256 dirty tiles; eligible RGBA8/mask tiles use a separately parity-gated WGSL compute kernel and fall back per operation to the exact CPU reference. RGBA64 stays on the 16-bit reference path.
- Every successful click commits one target-specific XOR tile-delta Undo entry and invalidates the existing session/tile surface without changing masks, groups, channels, project loading, SVG/vector/text behaviour or Hot/Warm/Cold residency. Added core tests for tolerance, contiguous/target-wide matching, selection feathering, transparent equivalence, channels, masks, hidden RGB, 16-bit arithmetic and tiled/reference parity.
- Public project format remains 14, vector schema remains 7, vector appearance schema remains 2 and private residency snapshots remain schema 15.

## 0.10.0h — Vector Appearance Presets (2026-08-01)

- Added a dedicated **Vector Appearance Presets** manager to the existing Appearance menus and Shape/Pen quick options. The manager supports saving the current selected-object appearance or current vector-tool defaults, applying presets, replacing an existing named preset, renaming and deleting.
- Presets retain the complete supported appearance payload: fill/stroke enabled states, colours, opacity, width, alignment, caps, joins, miter limit, solid/dashed settings, dash length/gap/offset and independent start/end arrowhead types and scales.
- Applying a preset to selected vector layers updates every contained vector object as one preflighted atomic Undo operation. With no vector selection, Shape and Pen tools accept the preset as their persisted new-vector defaults. Closed shapes continue to normalise inapplicable endpoint markers safely.
- Added a bounded, versioned `vfxphotolab-preset` user-data envelope with category `vector-appearance`, deterministic collision-resistant filenames, atomic `QSaveFile` writes, duplicate-name handling, malformed/oversized-file rejection and a 512-preset safety limit. The storage layout is intentionally compatible with later consolidation into the broader presets milestone.
- Added core regression coverage for the full appearance payload, overwrite, rename, deletion and the on-disk preset envelope. Public project format remains 14, vector schema remains 7, vector appearance schema remains 2 and private residency snapshots remain schema 15.

## 0.10.0g.2 — Pen Arrowhead Shaft-Clipping Fix (2026-08-01)

- Fixed start/end arrowheads cutting away unrelated middle sections of winding open Pen paths. The 0.10.0g.1 shaft-cap hardening intersected the complete stroke outline with an unbounded endpoint half-plane; any earlier segment lying in front of that plane disappeared even when it was far from the marker.
- Butt caps now require no clipping. Round and Square caps subtract only their bounded half-stroke endpoint footprint before the marker is united, preserving every distant segment while still preventing the cap from protruding through a pointed head.
- The corrected local operation is shared by settled rendering, bounds/tile coverage and Expand Stroke. Added regression coverage for both start and end arrows on self-returning open paths with Butt, Round and Square caps.
- Public project format remains 14, vector schema remains 7, vector appearance schema remains 2 and private residency snapshots remain schema 15.

## 0.10.0g.1 — Arrowhead Placement and UI Hardening (2026-08-01)

- Re-centred Open, Triangle, Stealth, Diamond and Circle markers on open-path endpoints. Pointed heads now extend beyond the centreline endpoint, and marker-bearing shaft caps are clipped at the endpoint plane so even minimum-scale heads cannot be overtaken or blunted by Round or Square caps.
- Updated SVG marker definitions to use the same centred reference geometry as native rendering, bounds calculation and Expand Stroke.
- Hid Start/End Arrow and scale rows for rectangles, rounded rectangles, ellipses, polygons, stars, filled Arrow shapes and closed Bézier paths. The Shape Tool no longer places disabled combo boxes inside its stroke popup, preventing Qt from dismissing the menu when an inapplicable control is clicked.
- New non-Line semantic shapes explicitly start without hidden arrowhead metadata, and changing a Line into a closed semantic shape clears its now-inapplicable marker values. Pen paths and Line shapes retain the existing independent start/end defaults.
- Added centred-marker geometry and SVG reference-point regression coverage. Public project format remains 14, vector schema remains 7, vector appearance schema remains 2 and private residency snapshots remain schema 15.

## 0.10.0g — Arrowheads (2026-08-01)

- Added nondestructive start and end arrowheads for open Pen paths and Line shapes. Start and end styles are independent and include **None**, **Open**, **Triangle**, **Stealth**, **Diamond** and **Circle**.
- Added independent 0.1×–10× arrowhead scaling relative to stroke width. Endpoint orientation follows the true first/last curve tangent after object, layer and nested-group transforms while the existing non-scaling document-pixel stroke behaviour remains intact.
- Integrated arrowheads into the Inspector and Shape/Pen Tool stroke popovers, tool defaults, Fill/Stroke appearance copy and paste, reset/swap safety, Undo/Redo, clipboard duplication, Image Size and Hot/Warm/Cold residency.
- Added standard SVG marker export plus exact Photo Lab metadata for lossless start/end style and scale import. Compatible SVG viewers receive marker definitions using stroke-relative units and `context-stroke` colour.
- Expanded strokes now include visible arrowhead geometry and bake it into ordinary editable filled Bézier contours together with the shaft.
- Added a semantic **Arrow Shape** to the Shape Tool family. It is a resolution-independent filled block arrow with editable head-length and shaft-width proportions, live marquee preview, Inspector controls, Convert Shape to Path, project/clipboard persistence and SVG export.
- Advanced public `.vfxphoto` format to 14, vector payload schema to 7, vector appearance schema to 2 and private residency snapshots to 15. Projects 1–13, vector schemas 1–6, appearance schema 1 and snapshots through schema 14 continue to load and migrate when their declared semantics are valid.
- Added regression coverage for rendering bounds, Expand Stroke, appearance migration, Image Size, semantic Arrow conversion, 16-bit project save/reopen, dishonest downgrade rejection and SVG marker/shape round trips.

## 0.10.0f.3 — Dashed Stroke Winding Fix (2026-08-01)

- Fixed the actual dashed **Expand Stroke** failure. The platform stroker uses nonzero winding, but 0.10.0f.1 stored every compound result as even-odd. Dash islands that touched or overlapped at corners or the closed-path seam therefore cancelled in the overlap during validation, causing the command to leave the original dashed stroke unchanged.
- Added an explicit persisted path fill rule. Expanded strokes use **Nonzero** so overlapping dash contours remain a union; ordinary existing paths retain the historic **Even-odd** default. Closed solid strokes still preserve inner holes because their outer and inner contours carry opposite winding.
- Removed the need to choose between disconnected contours and correct overlap semantics: dashed outlines remain spoke-free independent Bézier contours inside one editable path object, while the fill rule now matches the source geometry.
- SVG import/export now preserves both `evenodd` and `nonzero` path fills. Closed nonzero compound SVG paths no longer need to be split into separate layers merely to avoid changing their fill semantics.
- Advanced public `.vfxphoto` format to 13, vector payload schema to 6 and private residency snapshots to 14. Projects 1–12, vector schemas 1–5 and snapshots through schema 13 continue to load when their declared semantics are valid.
- Added regression requirements for dashed rectangle expansion under nonzero winding, schema downgrade rejection, project save/reopen and SVG fill-rule round trips.

## 0.10.0f.2 — Dashed Stroke Expansion Fix (2026-08-01)

- Fixed a regression introduced by 0.10.0f.1 where **Expand Stroke** could reject an entire dashed shape and leave it unchanged. Qt can emit duplicate or zero-area closure fragments at the seam of a closed dashed path; those non-rendering fragments are now filtered before Bézier validation instead of aborting every valid dash island.
- Visible dash contours remain mandatory and continue to become independent closed compound-path contours. The fix does not restore synthetic connector spokes, merge disconnected dashes, or reopen solid stroke rings.
- Area validation translates polygons to a local origin before measuring them, avoiding floating-point cancellation for small dashes positioned far from the document origin while retaining the existing malformed/excessive-geometry limits.
- Added regression coverage for closed dashed rectangles across positive, negative and boundary-adjacent dash offsets, requiring successful expansion, multiple independent contours and fill-coverage equivalence.
- Public `.vfxphoto` format remains version 12, vector payload schema remains 5, text schema remains 1, adjustment schema remains 6 and private residency snapshots remain schema 13.

## 0.10.0f.1 — Expand Stroke Contour Hardening (2026-08-01)

- Replaced the single-contour bridge workaround with explicit compound Bézier contours. Expanded dashed strokes now store each dash island independently, eliminating the spoke pattern, accidental long segments and the severe Direct Selection slowdown reported on dense dashed shapes.
- Closed solid strokes now preserve their outer boundary and inner hole as separate closed contours under an even-odd fill. Circle and other ring expansions therefore have genuine negative space with no artificial opening or anti-aliased bridge seam.
- Added active-contour editing for compound paths. Clicking any dash or hole boundary with Direct Selection or Corner Tool promotes that contour as the active ordinary Bézier path without changing the visible result or adding Undo history; node overlays remain limited to the active contour for responsive dense editing.
- Compound contours preserve stable node UUIDs, bounds, snapping, geometry-cache fingerprints, duplication IDs, Image Size scaling, JSON/project round-trip, Hot/Warm/Cold residency and SVG import/export. Explicitly even-odd closed SVG subpaths import as one editable compound object; nonzero-winding compounds keep the conservative split-and-warn fallback rather than silently changing fill semantics.
- Removed the fill-polygon/zero-area connector fallback. Pathological self-intersections now fail safely and leave the source stroke untouched rather than publishing visibly incorrect geometry.
- Deleting every node in the active compound contour removes only that contour and promotes another; the layer is removed only after its final contour/path object is deleted.
- Added regression coverage for independent dashed contours, clean closed-stroke holes, rendered equivalence, project and residency round-trips, SVG subpath closure, dishonest pre-compound metadata and project downgrade rejection.
- Public `.vfxphoto` format advances to version 12, vector payload schema to 5 and private residency snapshots to schema 13. Projects 1–11, vector schemas 1–4 and snapshots through schema 12 continue to load and migrate where their declared semantics are valid.

## 0.10.0f — Expand Stroke (2026-08-01)

- Added **Expand Stroke** to the Layer menu, vector-layer context menu and Direct Selection options bar. Every stroked object in the selected vector layers is processed atomically.
- The conversion resolves the actual visible stroke in document space before baking it back into editable layer-local geometry, preserving stroke width, inside/centre/outside alignment, butt/round/square caps, miter/round/bevel joins, miter limits, solid/dashed patterns, dash lengths, gaps and offsets across object, layer, group, affine and supported projective transforms.
- Expanded strokes become ordinary closed schema-4 Bézier paths with the former stroke colour and opacity stored as fill, no residual stroke and no object transform. Open strokes, closed rings, holes and disconnected dash islands remain one editable path through zero-area connector traversal; self-intersecting winding cases use a bounded fill-equivalent polygon fallback rather than publishing incorrect geometry.
- Preserved the complete pre-expansion appearance when a source also has a visible fill. The original vector layer is promoted to an Isolated group only when needed; fill and expanded-stroke child layers reproduce the original object draw order while the parent retains the original mask, opacity, blend mode, visibility and transform exactly once. Stroke-only single-object layers remain simple vector layers.
- Expanded outline child layers are selected after the command so Direct Selection can edit them immediately. The complete multi-layer operation records one Undo entry and rolls back to the untouched document if any geometry, transform or replacement validation fails.
- Added regression coverage for open dashed lines, centre-stroked ellipse rings, inside rectangles, outside stars, caps, joins, alignment, transforms, rendered equivalence, malformed/singular rejection, 16-bit project save/reopen and ordinary schema-4 JSON round-trip.
- Public `.vfxphoto` format remains version 11, vector payload schema remains 4, text schema remains 1, adjustment schema remains 6 and private residency snapshots remain schema 12.

## 0.10.0e — Vector Hover and Selection Feedback (2026-08-01)

- Added fixed-screen hover feedback for Direct Selection anchors, incoming/outgoing Bézier handles, path segments and Corner Tool live-corner controls. Hovered components use a high-contrast cyan/white treatment while selected anchors retain their yellow selection state, so hover and selection remain distinguishable.
- Pen Continue, Close and Join endpoint markers now use the same stronger hover halo while retaining their role-specific symbols and colours.
- Added component-aware cursors: move for anchors, pointing-hand for handles/corners/endpoints and crosshair for editable segments. Leaving the canvas, switching paths or beginning a pan clears stale hover state immediately.
- Improved dense-path hit testing with screen-space tolerances and weighted priority. Visible selected-node handles and live-corner controls win true overlaps, while a substantially closer anchor still wins instead of an offset handle being grabbed unexpectedly. Segment selection remains lower priority than any actionable control.
- Hovered curve segments are highlighted independently above the ordinary orange path overlay. All overlay sizes and hit radii remain viewport-pixel based, so low and high zoom produce consistent interaction targets without changing vector geometry or raster output.
- Added canvas regression coverage for hover-specific cursor changes, distinct rendered overlays, endpoint hover and clean hover reset.
- Public `.vfxphoto` format remains version 11, vector payload schema remains 4, text schema remains 1, adjustment schema remains 6 and private residency snapshots remain schema 12.

## 0.10.0d — Fill and Stroke Quick Actions (2026-08-01)

- Added **Swap Fill and Stroke** and **Reset Fill and Stroke** to the Edit and Layer menus, vector-layer context menu, and Shape, Pen, Direct Selection and Corner Tool top options. Multi-layer edits process every vector object in the selected vector layers as one atomic Undo operation.
- Swap exchanges fill/stroke enabled state, colour and opacity while retaining stroke-only geometry settings such as width, pattern, dash/gap/offset, caps, joins, alignment and miter limit. Open paths remain safely stroked and lines remain fill-free after normalisation.
- Reset restores a visible fill-only default for closed shapes and a centred stroke-only default for open paths/lines, using the current secondary colour for fill and current primary colour for stroke.
- Added **Copy Appearance** and **Paste Appearance** using a bounded, schema-tagged private clipboard payload with human-readable JSON fallback. The payload includes fill, stroke, both opacities, width, dash settings, cap, join, alignment and miter limit, but deliberately excludes geometry, transforms and object identity.
- Pasting applies the source appearance to every vector object in the selected vector layers, or to persisted Shape/Pen creation defaults when no vector layer is selected. New-vector fill/stroke opacities are now retained alongside the existing appearance defaults.
- Fixed middle-mouse panning in Pen, Direct Selection and Corner Tool. Active panning now takes pointer ownership before vector hover/edit dispatch, preventing those tools from consuming the pan move events.
- Added model regressions for appearance JSON round-trip, swap semantics, geometry/identity preservation, open-path canonicalisation, defaults and malformed payload rejection, plus a canvas regression proving middle-button panning does not emit vector-edit moves.
- Public `.vfxphoto` format remains version 11, vector payload schema remains 4, text schema remains 1, adjustment schema remains 6 and private residency snapshots remain schema 12.

## 0.10.0c — Keyboard Node Editing (2026-08-01)

- Added keyboard editing for Direct Selection and Corner Tool: Arrow keys nudge all selected anchors by one document pixel, while Shift+Arrow nudges by ten document pixels.
- Nudge deltas are mapped through inverse object, layer and group transforms, so screen-horizontal and screen-vertical movement remains exact for rotated, scaled, skewed, reflected, projective and nested paths. Anchors and active handles move together; node IDs, modes and live-corner metadata remain intact.
- Added Ctrl+A selection of every node in the active path and Escape-first deselection. These selection-only actions do not pollute document Undo history.
- Delete now removes selected nodes in both Direct Selection and Corner Tool. Deleting every node removes only the active path object when an imported vector layer contains sibling objects, and removes the layer itself only when the path was its sole object.
- Every nudge and deletion is recorded as one atomic document-state Undo entry for the complete selected node set. Invalid indices, singular transforms and vector-limit overflows are rejected without partial mutation.
- Added a model regression covering multi-node atomic movement, handle and UUID preservation, untouched unselected nodes, invalid-index no-ops and all-or-nothing safe-range rejection.
- Updated top-bar guidance and Fedora validation coverage. Public `.vfxphoto` format remains version 11, vector payload schema remains 4, text schema remains 1, adjustment schema remains 6 and private residency snapshots remain schema 12.

## 0.10.0b — Pen Path Continuation and Joining (2026-08-01)

- Added endpoint-aware Pen continuation from either the first or last anchor of any visible open path. Extending from the start prepends nodes without reversing the stored path, preserving dash direction and providing a stable foundation for later arrowheads.
- Clicking the opposite endpoint while drawing now closes the active path; clicking an endpoint on another open path joins the two paths. Donor geometry is mapped through its layer/object transforms into the active path's local space, with the active path's appearance retained deterministically.
- Added safe coincident-endpoint merging: touching endpoints become one editable junction, retain the active incoming handle and donor outgoing handle, and avoid zero-length duplicate nodes. Separated endpoints receive an ordinary connecting segment.
- Added fixed-screen endpoint feedback for Continue, Close, Join and the active drawing end, including hover emphasis and endpoint-priority hit testing so closure wins over overlapping join targets.
- Double-click and Enter finish an open path without closing it. Escape restores the pre-press document state when an anchor/segment gesture is still active, otherwise it simply finishes the Pen session. Backspace removes the newest anchor correctly whether extending from the start or end.
- Every added anchor, closure and path join remains an atomic Undo entry; cancelled live segments create no history entry. The consumed donor object is removed atomically, its layer is removed only when empty, sibling imported vector objects are preserved, and any replacement/removal failure rolls back the complete operation.
- Added model regressions for path-direction reversal, separated joining, coincident junction merging, handle/UUID preservation, safety rejection and geometry equivalence.
- Public `.vfxphoto` format remains version 11, vector payload schema remains 4, text schema remains 1, adjustment schema remains 6 and private residency snapshots remain schema 12.

## 0.10.0a — Convert Shape to Path (2026-08-01)

- Added an explicit **Convert Shape to Path** action to the Layer menu, vector-layer context menu and Direct Selection top options bar.
- Added conversion for Rectangle, Rounded Rectangle, Ellipse, Polygon, Star and Line objects, including every eligible object in a selected vector layer and multiple selected vector layers in one atomic operation.
- Preserved object/layer identity, fill, stroke, width, dash pattern and offset, caps, joins, alignment, miter limit, opacity and nondestructive transforms. Newly generated Bézier anchors receive stable unique node IDs and remain fully editable with Direct Selection and Corner tools.
- Rounded rectangles bake their current visible document-space corner outline before conversion, so document-pixel radii remain visually unchanged under supported non-uniform transforms instead of becoming stretched local radii.
- Added all-or-nothing conversion preflight, safe rollback on commit failure, refreshed overlays/thumbnails/Inspector state and singular-transform rejection without partial document mutation.
- Added focused regressions for every supported primitive, open/closed path state, appearance retention, visible raster equivalence, unique node identities, project JSON round-trip, no-op path conversion and malformed transform rejection.
- Public `.vfxphoto` format remains version 11, vector payload schema remains 4, text schema remains 1, adjustment schema remains 6 and private residency snapshots remain schema 12.

## 0.9.0g.4 — Pen Tool appearance options QoL (2026-07-31)

- Brought the Pen Tool top options bar to parity with the Shape tools: Fill and Stroke toggles, secondary-fill and primary-stroke colour swatches, document-pixel stroke width and the shared Solid/Dashed stroke popup are now available before and during path creation.
- Exposed the same dash length, gap, offset, cap, join, alignment and miter-limit defaults used by semantic shapes, while retaining Finish Path and Close Path in the same bar.
- New Pen paths preserve the current fill preference while open, use the current stroke width/pattern/cap/join defaults immediately, and atomically apply the current closed-path Fill/Stroke toggles and alignment when closed with the Pen Tool.
- Reused the existing persistent vector creation settings, primary/secondary colour slots and project/SVG stroke semantics; no document-format, vector-schema, text-schema or residency-schema changes were required.

## 0.9.0g.3 — Vector stroke and creation defaults QoL (2026-07-31)

- Changed new vector styling defaults so closed-shape fills use the current secondary colour and strokes use the current primary colour. The Shape top bar edits those same global colour slots rather than maintaining a disconnected private stroke colour.
- Added a bundled placeholder glyph for the Corner Tool so its Pen/Direct Selection dropdown and toolbar action are never blank and can be replaced cleanly by a future custom icon set.
- Added semantic Solid/Dashed vector strokes with document-pixel dash length, gap length and offset, plus Butt, Round and Square cap choices. Cap style applies to each individual dash as well as open path ends.
- Added a compact stroke-style popup to the Shape top bar and matching Inspector controls for existing vector layers, including style, dash, gap, offset, cap, join, alignment and miter limit. New-path settings persist between launches.
- Preserved dashed strokes through Undo/Redo, project save/reopen, vector clipboard transfer, duplication, Image Size, Hot/Warm/Cold residency and SVG import/export. Complex external SVG dash sequences are imported using their first dash/gap pair with an explicit warning.
- Advanced vector payload schema to 4, public `.vfxphoto` format to 11 and private residency snapshots to schema 12. Versions 1–10 and vector schemas 1–3 remain supported; dishonestly relabelled older payloads containing dashed strokes are rejected.
- Added focused model, geometry, scaling, project-version and SVG dash/cap round-trip regressions.

## 0.9.0g.2.2 — True sharp-node conversion (2026-07-31)

- Added **Make Sharp** to Direct Selection's top options bar and the vector-path Inspector. It collapses both incoming and outgoing handles exactly onto each selected anchor, changes the node to Corner mode and clears any live-corner radius so the result is a genuine sharp point eligible for the Corner Tool.
- Retained **Make Corner** as the separate handle-decoupling operation: it preserves existing handle lengths while allowing each side to be edited independently.
- Added a focused vector-node regression covering handle collapse, Corner mode and live-corner reset.
- No project-format, vector-schema, text-schema, residency-schema or SVG-format changes.

## 0.9.0g.2.1 — ImageCanvas declaration build fix (2026-07-31)

- Declared `ImageCanvas::selectionMarqueePreviewMode() const` in `ImageCanvas.h`, matching the implementation and regression-test API added in 0.9.0g.2.
- No document-format, vector-schema, text-schema, residency-schema, SVG or interaction behaviour changes.

## 0.9.0g.2 — Vector Interaction QoL (2026-07-31)

- Added explicit **Make Corner** and **Make Smooth** operations to Direct Selection's top bar and Inspector. Corner conversion decouples existing handles; Smooth creates or realigns opposite handles using neighbouring path geometry while preserving unequal handle lengths.
- Added absolute 45-degree snapping while dragging incoming or outgoing handles with Shift in Direct Selection. The constraint is measured from the document axes, never relative to the handle's previous angle.
- Added live rectangle-marquee node selection to Direct Selection and Corner tools. Dragging empty canvas space replaces the node selection; Shift-drag adds enclosed anchors/cornerable nodes to it.
- Fixed raster Rectangle Select visually reusing the last vector Shape preview (ellipse, rounded rectangle, polygon, star or line) when the internal ellipse flag had not changed.
- Added a canvas regression covering vector-shape preview reset when returning to Rectangle/Ellipse selection.
- No project-format, vector-schema, text-schema or residency-schema changes.

## 0.9.0g.1 — Corner Tool Build Fix (2026-07-31)

- Fixed the release-build failure in `VectorBezierPath::painterPath()` by capturing the containing object instead of attempting to capture the `nodes` data member directly in the live-corner drawing lambda.
- No project-format, vector-schema, residency-schema or behaviour changes from 0.9.0g.

## 0.9.0g — Live Vector Corner Tool (2026-07-31)

- Added a dedicated **Corner Tool** to the existing Pen/Direct Selection right-click family, with fixed-screen canvas handles and path overlays that follow the generated cornered outline.
- Added non-destructive per-node live corners for eligible sharp nodes on closed paths, with Rounded, Chamfer, Concave and Cutout styles. Shift-selected corners share one drag, and the top options bar provides exact radius/style controls, Select All Corners, Clear and Bake Corners.
- Added on-demand semantic Rectangle, Polygon and Star conversion to editable paths when targeted by the Corner Tool. Fill, stroke, opacity, transforms, masks, groups and layer identity remain intact; rasterisation is never used as the conversion source.
- Added Bake Corners, converting the current generated outline into ordinary Bézier anchors and handles for unrestricted Direct Selection editing. Node-mode or handle edits clear incompatible live-corner data explicitly.
- Advanced vector payload schema to 3, public `.vfxphoto` format to 10 and private residency snapshots to schema 11. Versions 1–9, vector schemas 1–2 and earlier snapshots remain supported; dishonestly relabelled old payloads containing live corners are rejected.
- Preserved live-corner values through Undo/Redo, project save/reopen, editable vector clipboard transfer, Image Size, transforms, Hot/Warm/Cold residency and exact Photo Lab SVG metadata. Standard SVG output receives the visible generated geometry so external viewers do not depend on private metadata.
- Added focused geometry, style, schema migration, baking, project-version and malformed-version regression coverage.

## 0.9.0f — Text and Vector Integration and Hardening (2026-07-31)

- Closed the 0.9.0 Text and Vector Foundation milestone without changing public `.vfxphoto` version 9, vector schema 2, text schema 1, adjustment schema 6 or private residency schema 10.
- Added whole-document SVG import budgets for editable leaves, vector objects, Bézier nodes, text characters and layer-tree nodes. Oversized but otherwise parseable files now stop admitting additional semantic content at deterministic limits instead of expanding per-element limits into unbounded document memory.
- Added defensive SVG export preflight for hierarchy depth, tree size, semantic object/node/text budgets, transforms, opacity and vector/text payload safety before XML generation or file replacement. `QSaveFile` remains the atomic publication boundary.
- Corrected SVG container visibility semantics: `visibility:hidden` remains inherited by descendants but a child can explicitly restore `visibility:visible`; `display:none` still suppresses the complete group. Root and nested SVG containers follow the same rule.
- Stopped rendering `<symbol>` definitions as ordinary artwork when `<use>` is unsupported, reports `<switch>` approximation explicitly, validates arc flags strictly, diagnoses malformed root/nested viewport dimensions, preserves supported element/root `mix-blend-mode` values and reports unsupported/non-scaling vector effects honestly.
- Hardened text interoperability: invisible fill/stroke-free text is skipped, stroke-only text is converted visibly and reported, fill-plus-stroke text preserves its fill with a limitation warning, requested font style is retained, and text-on-path, per-span styling and per-glyph offsets/rotation are surfaced rather than silently flattened.
- Exported closed semantic paths now declare `fill-rule="evenodd"` to match the editor's path-fill behaviour in external SVG viewers. Damaged `data-vfx` metadata continues to fall back to ordinary SVG geometry.
- Integrated SVG operations with active canvas text editing, and disabled document/selection SVG export actions when no semantic vector or text content is available, avoiding stale drafts and pointless save dialogs.
- Expanded `VFXPhotoLabSvgTests` across visibility overrides, element blend modes, symbol handling, invisible/stroke-only text, damaged-metadata fallback, nested selected-root transforms, external fill-rule output, strict arc flags, cumulative import/export budgets and unsafe-export rejection.

## 0.9.0e — SVG Workflow (2026-07-31)

- Added a dedicated File → SVG workflow with **Open SVG as Document**, **Import SVG as Layers**, **Export SVG Document** and **Export Selected Layers as SVG**. Imported artwork remains semantic and editable instead of being rasterised.
- Added bounded SVG XML parsing without a Qt SVG runtime dependency. Imports reject DTD/entity input, cap file size, element count, nesting depth, path nodes and coordinates, and skip unsafe or unbounded objects without corrupting the remaining document.
- Added practical external-SVG support for root and nested `viewBox`/viewport mapping, `preserveAspectRatio`, groups, affine transform lists, inherited inline/presentation styles, opacity, blend-mode hints, rectangles, circular and elliptical rounded rectangles, circles/ellipses, lines, arbitrary polygons/polylines, text and Bézier paths.
- Added complete path command handling for absolute/relative M, L, H, V, C, S, Q, T, A and Z commands. Quadratic and elliptical-arc segments convert deterministically into editable cubic handles; compound paths are split into editable subpath layers with a visible warning where hole winding may differ.
- Added standard SVG export for semantic vector and text layers, nested groups, layer/object transforms, solid fills, strokes, caps, joins, opacity and text lines. Full-document export omits unsupported raster/adjustment layers explicitly; selected-layer export detaches selected roots with their accumulated document transforms.
- Added compact `data-vfx` metadata alongside ordinary SVG geometry so Photo Lab-generated files round-trip exact vector/text semantics, including independent corner radii, regular polygon/star parameters, path UUID-safe geometry, document-pixel strokes, requested fonts and area-text settings, while remaining viewable and editable in other SVG applications.
- Added focused SVG import/export regression coverage. Public `.vfxphoto` remains version 9, vector payload schema remains 2, text payload schema remains 1 and private residency snapshots remain schema 10.

## 0.9.0d.1.1 — Release Build Fix (2026-07-30)

- Fixed a release-build const-correctness error in `interactivePreviewEditActive()` by adding a read-only `DocumentSession::propertyUndoActive() const` accessor while retaining the mutable reference accessor used by edit transactions.
- No rendering, persistence, interaction or project-format behaviour changed.

## 0.9.0d.1 — Vector Interaction Performance (2026-07-30)

- Replaced pointer-rate whole-layer copying, full-path normalisation and deep equality checks with a known-changed semantic mutation path. Direct Selection now detaches once per gesture and updates only selected anchors or the active handle; release still performs authoritative normalisation and records one atomic Undo command.
- Cached canvas, guide, layer and vector snap targets for the duration of each node gesture instead of traversing the complete layer tree on every mouse event.
- Added a globally bounded transformed-vector geometry cache. Base paths, stroke outlines and analytic bounds are reused across tiles, while expensive boolean path unions are avoided for culling. Cache keys include semantic fingerprints, revisions and world transforms, and focused long-path regressions verify tile equivalence and edit invalidation.
- Generalised the sharp interactive preview scheduler to Pen and Direct Selection gestures: pointer bursts are coalesced into one visible level-0 request, in-flight frames may finish in order instead of being repeatedly cancelled, and stale or post-release generations remain rejected.
- Made pure vector/text layer translation use the existing sharp foreground bitmap directly in the canvas, eliminating synchronous semantic rerasterisation on every movement event. Scale, rotation, text-box resize, document-pixel strokes and rounded corners retain semantic rendering.
- Bounded non-translation live vector rerenders to the selected semantic content region instead of the complete document. Public `.vfxphoto` remains version 9, vector schema remains 2 and private residency snapshots remain schema 10.

## 0.9.0d — Pen and Node Editing (2026-07-30)

- Added schema-2 semantic Bézier paths with stable node UUIDs, open/closed state, anchors, optional incoming/outgoing handles and Corner, Smooth or Symmetric modes. `QPainterPath` is generated for rendering/hit testing but is not persisted as the source of truth.
- Added Pen and Direct Selection tools in a shared right-click family with `P` and `Shift+P` shortcuts. Click creates straight anchors, drag creates symmetric handles, Shift constrains placement/handles to 45-degree increments, first-anchor click closes and double-click/Enter finishes an open path.
- Added fixed screen-size path overlays, path-layer picking, multi-node Shift selection, anchor/handle dragging, exact De Casteljau segment insertion, node deletion, mode conversion, selected-endpoint joining and path Open/Close commands in the top bar and Inspector.
- Integrated path anchors into existing guide/canvas/layer/vector snapping, transforms, masks, blend modes, Isolated/Pass Through groups, thumbnails, Alpha-to-selection, editable vector clipboard transfer, Image Size and Hot/Warm/Cold residency.
- Preserved exact curve geometry during normalisation and node insertion; Smooth/Symmetric constraints are applied only by explicit handle edits or mode conversion. Added bounded adaptive curve hit testing and malformed/oversized path rejection.
- Advanced public `.vfxphoto` to version 9, vector payload schema to 2 and private residency snapshots to schema 10 while retaining public versions 1–8 and vector schema 1. Open paths retain their closed-path fill and stroke-alignment preferences non-destructively.
- Added persistence, dishonest-version rejection, mode/curve-insertion, RGBA8/RGBA64 rasterisation, tile-region, snapping, UUID regeneration, Image Size and residency regressions.

## 0.9.0c.4 — Canvas Text Empty-State and Crash Hardening (2026-07-30)

- Fixed the actual empty-text size mismatch: the application-wide `QWidget { font-size: 10pt; }` stylesheet was overriding `QTextEdit::setFont()`. The canvas editor now pins its semantic zoomed font size in a more-specific `CanvasTextEditor` stylesheet, so the first glyph typed into an empty layer matches the committed rendering immediately.
- Removed document-format rewriting from the canvas editor's `textChanged`, key-input and IME mutation paths. Live edits now update geometry only; this avoids re-entrant `QTextDocument` layout/cursor mutation that could crash after deleting all text and reopening the editor.
- The editor still receives the layer's resolved family/style, weight, italic state, tracking, leading, alignment and RGBA colour when editing begins or typography settings change.
- Public `.vfxphoto` remains version 8, text schema remains 1 and private residency snapshots remain schema 9.

## 0.9.0c.3 — Empty-Text Typing Format Hardening (2026-07-30)

- Fixed the canvas editor losing its current character format when the final glyph was deleted. Empty text now immediately reacquires the layer’s resolved font, document-pixel size, weight, italic state, tracking, colour, Alpha, leading and alignment.
- Refreshes the semantic typing format before replacing the complete document, including the initially selected placeholder in a newly created text layer. The first replacement character therefore matches the committed rendering instead of inheriting Qt’s smaller default editor font.
- Applied the same pre-insertion guard to IME/composition input as ordinary key events.
- Public `.vfxphoto` remains version 8, text schema remains 1 and private residency snapshots remain schema 9.

## 0.9.0c.2 — Canvas Text Fidelity and Tool-State Hardening (2026-07-30)

- Fixed the in-canvas editor scale calculation by mapping local text basis vectors through the authoritative layer transform and then the canvas mapping. Layer translation can no longer contaminate the measured scale, so entering edit mode no longer makes text appear smaller.
- Applied the text layer’s resolved family/style, document-pixel font size, weight, italic state, tracking, leading, alignment, RGBA colour, text opacity and layer opacity to both existing glyphs and newly typed characters in the live editor.
- Overrode the global themed `QTextEdit` foreground specifically for the canvas editor. Theme text colours can no longer turn edited text white or otherwise replace the semantic layer colour.
- New text layers explicitly initialise from the current primary colour, including Alpha, with a safe black fallback only if the active colour is invalid.
- The Inspector’s Edit Text on Canvas action now activates the Text tool and enters the same editing state as clicking existing text in the image. Any subsequent switch to a non-Text tool commits and closes the editor reliably.
- Canvas zoom changes now update the live editor’s typography scale as well as its geometry; ordinary panning only repositions it. Public `.vfxphoto` remains version 8, text schema remains 1 and private residency snapshots remain schema 9.

## 0.9.0c.1 — Direct Text Editing and Text-Box Behaviour (2026-07-30)

- Added direct canvas text editing for new and existing text layers. Clicking existing text with the Text tool now edits that layer instead of creating another one; ordinary Enter inserts explicit lines, Ctrl+Enter commits and Escape cancels.
- Replaced the Inspector text-content box and Apply button with a canvas editor. Draft text is shown live over the image, while the matching semantic layer is temporarily omitted from preview snapshots to prevent duplicate rendering.
- Added shared multiline measurement and rendering for point text and area text, including explicit blank lines, wrapped paragraphs and automatic area-height growth.
- Added text-aware transform resizing: horizontal handles change the layout box without scaling glyphs, alignment remains meaningful inside the box, vertical contraction reduces font size only as far as required to fit, and vertical expansion never enlarges the type.
- Typography, alignment, overflow, colour and text-box Inspector controls now update immediately. Inspector focus no longer ends canvas editing, and panel rebuilding restores the previous scroll position.
- Saving, exporting, Copy Merged and document switching commit the active text draft first, while ordinary Copy/Cut/Paste continue to route to the focused canvas text editor.
- Replaced the full-background Text Colour button with the same contained swatch-icon convention used by vector fill and stroke controls.
- Added offscreen text-layout/rasterisation regressions for explicit newlines, Auto Height and shrink-to-fit font sizing. Public `.vfxphoto` remains version 8, text schema remains 1 and private residency snapshots remain schema 9.

## 0.9.0c — Editable Text Layers (2026-07-30)

- Added resolution-independent Point and Paragraph/Area Text layers with semantic content, requested font identity, font size, RGBA colour, alignment, tracking, leading and Auto Height or Clip overflow.
- Added deterministic requested-region text layout and antialiased RGBA8/RGBA64 rasterisation feeding the existing CPU/GPU hierarchy compositor, masks, groups, transforms, thumbnails, selections and clipboard paths.
- Added missing-font fallback reporting while preserving the requested family and style in project data.
- Advanced public `.vfxphoto` to version 8 and private Hot/Warm/Cold snapshots to schema 9 while retaining versions 1–7.

## 0.9.0b.2 — Deferred Shape-Scale Validation (2026-07-30)

- Disabled keyboard tracking for semantic Width/Height and layer-scale Inspector fields so partially typed values no longer mutate or reject the shape before the user commits the entry. Enter, focus loss and spin-button stepping remain commit points.
- Replaced dynamic spin-box minimums with commit-time rounded-corner validation. Width and Height values below the legal document-pixel corner geometry now snap to the nearest permitted dimensions as one atomic edit.
- Layer-scale entries that would collapse a rounded rectangle now binary-search the valid interval from the current transform to the requested transform and commit the closest legal scale instead of restoring the previous value. This allows ordinary edits such as 101 → 100 without workarounds.
- Public `.vfxphoto` remains version 7, vector schema remains 1 and private residency snapshots remain schema 8.

## 0.9.0b.1 — Parameterised Shape Hardening (2026-07-30)

- Fixed Inside-aligned strokes on concave Stars and Polygons by using the semantic fill path as conservative tile/transform coverage and clipping a doubled centre outline during rasterisation instead of relying on fragile path-union bounds. This prevents cut tiles, displaced transform boxes and intermittent missing stroke sections.
- Applied the same robust Inside-stroke clipping to vector thumbnails. Centre and Outside stroke behaviour is unchanged.
- Shape creation constraints now follow live modifier state throughout the drag: Shift keeps Rectangle, Rounded Rectangle, Ellipse, Polygon and Star at 1:1, and constrains Line to exact 45-degree increments even when pressed after drawing has begun. Alt-from-centre remains live in the same way.
- Deleting selected layers now selects the nearest surviving visual neighbour, preferring the layer immediately below and then the layer above, instead of always jumping to the bottom/Base layer. Nested groups and multi-layer deletion skip removed descendants safely.
- Added regressions for concave Inside-stroke bounds/tiled equivalence and live marquee modifier changes. Public `.vfxphoto` remains version 7, vector schema remains 1 and private residency snapshots remain schema 8.

## 0.9.0b — Complete Parameterised Shapes (2026-07-30)

- Added semantic Line, Polygon and Star objects alongside Rectangle, Rounded Rectangle and Ellipse.
- Added explicit line endpoints, 3–64 polygon/star points, star inner-radius ratio and editable vertex rotation.
- Added independent Fill and Stroke enablement, RGBA colours and opacity.
- Added document-pixel stroke width; Inside/Centre/Outside alignment; Butt/Round/Square caps; Miter/Round/Bevel joins; and bounded miter limits. Inside and Outside widths occupy their complete declared side rather than half-width clipping.
- Expanded the Shape tool family, live drag preview and Inspector for every new parameter. Shift constrains lines to 45-degree increments and Alt creates from the centre.
- Added semantic vertex/centre snapping to the existing canvas, guide and layer snapping system.
- Added bounded editable vector copy/cut/paste with private MIME data, raster interoperability previews, regenerated layer/object UUIDs and document-space placement preservation inside transformed groups.
- Extended RGBA8/RGBA64 tile rasterisation, culling, thumbnails, selections, masks, group compositing, transforms, Image Size and content bounds to include fill and stroke geometry.
- Retained public `.vfxphoto` version 7, vector payload schema 1 and private residency schema 8. All 0.9.0a vector payloads migrate without loss.
- Added focused regression coverage for parameter persistence, malformed data, stroke geometry/precision, snap points, copy insertion and semantic Image Size scaling.

## 0.9.0a.2 — Live Shape Radius, Pivot and Colour Hardening (2026-07-30)

- Rounded-rectangle corner radii are now interpreted as document-pixel values during ordinary scale and rotation rendering. Shapes with the same Inspector radius retain matching visible corners even when their layer scales differ.
- Added scale-aware minimum live-shape size guards based on linked or per-corner adjoining-radius sums. Shape creation, semantic Width/Height controls and ordinary layer/group transform sessions reject or clamp dimensions that would collapse the final document-space body below its corners; skew and perspective remain explicit full-path deformations.
- Fixed Inspector rotation and scale replacement for vector layers so the semantic content centre remains fixed instead of rotating around the document origin and jumping away.
- Added double-click RGBA colour chooser access for the primary colour, secondary colour, populated saved swatches and empty saved swatches. Existing single-click activation and right-click Save/Clear behaviour remain intact.
- Stopped semantic payload normalisation from clipping document-pixel radii against pre-transform local bounds, while retaining bounded malformed-data handling and render-time safety normalisation.
- Expanded rounded-corner regression coverage for constant radii under uniform and non-uniform transforms, transformed-local geometry, safe initial creation, minimum-size calculations and transform rejection. Public `.vfxphoto` remains version 7, vector schema remains 1 and private residency snapshots remain schema 8.

## 0.9.0a.1 — Vector Inspector and Transform Hardening (2026-07-30)

- Fixed vector layers incorrectly falling through to the Exposure adjustment Inspector.
- Added linked or independent top-left, top-right, bottom-right and bottom-left rounded-rectangle radii while retaining compatibility with the original uniform `cornerRadius` payload.
- Rounded corners now remain circular under ordinary non-uniform scale and rotation instead of becoming elliptical. Explicit skew and perspective continue to deform the complete path.
- Shape geometry, fill opacity and corner-radius edits now invalidate and redraw immediately, including while the Transform tool is selected, while each completed gesture remains one Undo operation.
- Inspector position, scale and rotation changes now update the authoritative layer transform immediately so the next canvas drag starts from the displayed values.
- Pure layer translation now commits automatically on mouse release, keeps the normal non-orange transform outline and does not require Apply. Scale, rotate, skew, distort, perspective and selected-pixel transforms retain the explicit pending Apply workflow.
- Added semantic live vector transform previews, no-double-transform canvas coverage and focused per-corner persistence/circular-radius regressions. Public `.vfxphoto` remains version 7, vector schema remains 1 and private residency snapshots remain schema 8.

## 0.9.0a — Resolution-Independent Layer Core (2026-07-29)

- Added semantic Vector Shape layers with Rectangle, Rounded Rectangle and Ellipse objects. Geometry, object transforms, RGBA64 fill colour, fill opacity, UUIDs and revisions remain resolution-independent in project data.
- Added a Photoshop-style Shape tool family with right-click subtype selection, `U`/`Shift+U` shortcuts, off-canvas creation, Shift-constrained proportions, Alt-from-centre drawing and live rounded/ellipse outlines.
- Added Vector Shape Inspector controls for type, fill colour/opacity, document-space geometry and corner radius, plus vector layer thumbnails and transparency-to-selection support.
- Added deterministic bounded CPU tile rasterisation for RGBA8 and RGBA64. Rasterised vector tiles feed the existing native WebGPU hierarchy compositor, preserving masks, opacity, blend modes, Isolated/Pass Through groups, cancellation, stale-result rejection and hidden-RGB reference behaviour.
- Added analytic vector content bounds to transforms, snapping, Canvas Reveal/Fit, selected-layer fitting and Image Size. Image resizing scales semantic geometry without flattening.
- Advanced public `.vfxphoto` to version 7 and private Hot/Warm/Cold snapshots to schema 8. Existing public versions 1–6 and private schema 7 remain readable; pre-version-7 files containing vector payloads are rejected rather than silently mislabelled.
- Added bounded vector geometry/tile cache accounting, duplication UUID remapping, malformed-data validation and focused persistence, residency, compositing, precision, cache, cancellation, Canvas Fit and Image Size regressions.

## 0.8.0f — Shadows/Highlights and Hardening (2026-07-29)

- Added a non-destructive Shadows/Highlights adjustment with Shadow Amount/Width, Highlight Amount/Width, Radius, Midtone Contrast and Colour Correction, plus built-in balanced/strong recovery presets.
- Added exact RGBA8/RGBA64 CPU local-adaptation processing and a native two-pass horizontal/vertical WGSL path. Alpha is unchanged and straight hidden RGB remains preserved.
- Added cumulative spatial dependency halos to CPU region rendering, native interactive viewport rendering, tiled GPU rendering and hidden-RGB export/reference paths. Consecutive spatial adjustments and group boundaries render without 256-pixel seams.
- Advanced typed adjustment payloads to schema 6 while keeping public `.vfxphoto` version 6 and private Hot/Warm/Cold snapshot schema 7.
- Expanded feature-specific GPU validation to all sixteen adjustments and added persistence, precision, tile-boundary, cancellation and native parity regressions for Shadows/Highlights.
- Completed the 0.8.0 Colour and Tonal Workflow Expansion milestone; the next planned milestone is Text and Vector Foundation.

## 0.8.0e.1

- Fixes the Fedora compile failure in the native LUT preparation paths by including `CubeLut.h` in both `WebGpuContext.cpp` and `TiledCanvasEngine.cpp`.
- No project-format, adjustment-schema, rendering-formula or preset changes from 0.8.0e.

## 0.8.0e

- Added non-destructive Posterise with 2–256 component levels and Threshold with Luminance/Red/Green/Blue sources plus exact 8/16-bit code-value controls.
- Added embedded `.cube` LUT adjustments supporting 1D shapers, 3D cubes and combined shaper+3D files, independent input domains, trilinear interpolation and 0–100% Strength. Projects and LUT presets remain functional after the original file is removed.
- Added native tiled WGSL paths and feature-specific startup validation for Posterise, Threshold and display-range LUTs. The adjustment uniform block is now 576 bytes and the diagnostic chain covers all fifteen adjustment types. Extended-range or oversized LUTs select the exact CPU reference without disabling unrelated GPU features.
- Added built-in and user presets to every adjustment inspector. User presets are versioned per-type JSON under the application data directory; applying a preset creates one atomic Undo entry, and LUT presets embed their table.
- Advanced typed adjustment payloads to schema 5 with schema 1–4 and legacy migration. Public `.vfxphoto` remains version 6 and private Hot/Warm/Cold snapshots remain schema 7.
- Added bounded `.cube` parsing, compressed embedded-vector validation, deterministic LUT fingerprints, stale-result protection and regression coverage for discrete precision, malformed files, embedded round trips, extended-range fallback and preset persistence.


## 0.8.0d.1

- Fixed Gradient Map stop drags remaining logically active after the pointer left the editor or mouse capture was lost. Dragging now owns mouse capture explicitly and terminates on release, ungrab, hide, disable or window deactivation.
- Prevented the separate Stop Position slider from being continuously repositioned underneath the pointer while a gradient-bar drag is active; it synchronises once when the gesture finishes.
- Hardened the shared slider control so a stale slider-down state cannot turn ordinary hover movement into an unintended value change.
- Replaced the Stop Colour button's border-based colour strip with a contained 16×16 colour swatch icon that respects the normal button frame in Midnight, Graphite and Daylight.
- Added offscreen widget regressions for lost gradient mouse capture, stale slider hover and the contained colour swatch. No project, adjustment or residency schema changes.

## 0.8.0d

- Added non-destructive Channel Mixer adjustment layers with independent Red, Green and Blue output matrices, constants and a separate monochrome mixture.
- Added Black and White conversion with six colour-family contribution controls and optional tint Hue/Saturation.
- Added an interactive Gradient Map editor with up to 64 ordered stops, direct colour editing, numeric stop positions, Reverse and Linear/Smooth/Constant interpolation.
- Extended the shared tonal lookup cache so Gradient Map uses exact 256-entry RGBA8 and 65,536-entry RGBA64 luminance mappings without rebuilding per tile.
- Added native tiled WGSL paths and feature-specific startup parity validation for Channel Mixer, Black and White and Gradient Map. The adjustment uniform block is now 480 bytes and the diagnostic chain covers twelve adjustment types.
- Advanced typed adjustment payloads to schema 4 with schema 1–3 and legacy migration. Public `.vfxphoto` remains version 6; private Hot/Warm/Cold snapshots remain schema 7.
- Preserved masks, opacity, blend modes, Isolated/Pass Through groups, straight Alpha, hidden RGB, cancellation, stale-result rejection and one atomic Undo entry per interaction.
- Added public-project, private-residency, 8/16-bit, hidden-RGB and tonal-behaviour regression coverage.

## 0.8.0c.6

- Fixed the native single-submit viewport crash when opening a document whose opaque bottom raster was promoted directly to the GPU accumulator. Uploaded hierarchy textures now always include `COPY_SRC`, matching the final texture-to-buffer readback performed by the compositor.
- Added a startup parity case for the exact single-raster hierarchy path used by a newly opened image, so a missing texture usage capability is detected before the main process begins rendering documents.
- Corrected the diagnostic documentation: the application-owned uncaptured-error callback improves reporting, but invalid queue submissions can still be fatal inside wgpu-native and must be prevented through valid resource usage.
- Retains all 0.8.0c.5 WGSL validation, sharp interactive rendering, adjustment schema 3, private residency schema 7 and public `.vfxphoto` version 6 behaviour.

## 0.8.0c.5

- Removed the remaining WGSL-reserved `match` identifier from the selective-colour adjustment shader. Both the external shader and embedded runtime copy now use `lightness_offset`.
- Added a complete build-time WGSL reserved-word scan across every external `.wgsl` file and every embedded `R"WGSL(...)WGSL"` source block. Linux builds now stop before CMake if a current WGSL reserved token is present.
- Added the same reserved-word preflight to the common runtime shader-module factory so malformed embedded or external shader text is rejected before reaching wgpu-native.
- Installed an explicit uncaptured WebGPU error callback on the device. Validation failures are now recorded and reported by Photo Lab rather than invoking wgpu-native's default Rust panic handler and aborting the isolated diagnostic helper.
- Kept GPU feature rejection scoped: a failed shader or pipeline disables the affected native path and leaves the exact CPU fallback available.
- No project-format, adjustment-schema, adjustment-formula, history, residency or sharp-interaction changes from 0.8.0c.4.

## 0.8.0c.4

- Removed the WGSL-reserved `target` identifier from both copies of the selective-colour adjustment shader.
- Restored parsing past the first reported wgpu-native 29.0.1.1 error, but another reserved local identifier (`match`) remained and still caused the isolated helper to abort before GPU approval. This is fully corrected and guarded in 0.8.0c.5.
- No project-format, adjustment-schema, formula, history, residency or interactive-preview changes from 0.8.0c.3.

## 0.8.0c.3

- Removed coarse mip-level adjustment previews. Slider, Curves and layer-opacity gestures now retain level-0 detail throughout and never alternate between blurred and sharp canvas presentations.
- Added a dedicated single-submit interactive viewport compositor. Eligible 8-bit visible regions prepare the exact current hierarchy once, encode the full viewport in one native WebGPU command buffer and perform one readback instead of serial submit/map cycles for every 256-pixel tile.
- Kept complete-frame atomic publication: the previous sharp viewport remains visible until the full replacement region for one parameter state is ready. Partial or mixed-value frames are never painted.
- Changed high-rate interaction ordering so the one currently executing frame may publish while newer pointer values coalesce. Because preview work is single-flight, accepted interaction generations remain monotonic; the latest coalesced value starts immediately afterwards, while frames are rejected once the gesture ends.
- Preserved the bounded tiled GPU/CPU renderer as automatic fallback for exact RGBA64 documents, unavailable or unapproved GPU features, and viewport hierarchies that exceed the single-submit 256 MiB working-set guard.
- Reduced per-frame GPU transfer and composition overhead: unmasked passes now use a 1×1 placeholder mask that is never sampled, transparent accumulators are allocated lazily, and a fully opaque unmasked bottom Copy layer becomes the initial accumulator without an unnecessary clear-texture upload or composite pass.
- Added regression coverage for ordered interactive publication and equivalence between the interactive fallback and authoritative tiled rendering. Public `.vfxphoto` remains version 6, adjustment schema remains 3 and private residency snapshots remain schema 7.

## 0.8.0c.2

- Tightened interactive canvas publication so superseded generations and request serials could not overwrite a newer accepted state.
- Added a regression helper covering exact settled publication identity.
- No project-format, adjustment-schema or adjustment-formula changes from 0.8.0c.1.

## 0.8.0c.1

- Added a reusable slider-plus-number adjustment control with direct typing, one-step arrow changes and one atomic Undo transaction per completed interaction. Existing scalar adjustment controls, targeted Hue/Saturation ranges and all three Colour Balance axes now share it.
- Removed repeated full-document history snapshots from intermediate property values; the complete before-state is captured once at interaction start and committed once at interaction end.
- Added coalesced progressive interactive previews: zoom-appropriate tiled results can publish during a drag, current GPU work is allowed to finish instead of being continually starved by cancellation, and a sharp atomic level-0 render follows release.
- Deferred document thumbnails, residency enforcement and document-strip refresh until the adjustment interaction settles.
- Split native adjustment validation into per-type approval for Exposure, Contrast, Saturation, Levels, Curves, Hue/Saturation, Vibrance, White Balance and Colour Balance. A cumulative mixed-chain rounding delta is now diagnostic only and cannot disable unrelated validated adjustments.
- Public `.vfxphoto` remains version 6, typed adjustment payloads remain schema 3 and private residency snapshots remain schema 7.

## 0.8.0c

- Added non-destructive Hue/Saturation adjustment layers with master Hue/Saturation/Lightness and six independently editable targeted colour ranges. Each range exposes centre, core width and feather controls for difficult or overlapping colours.
- Added adaptive Vibrance with a separate Saturation control, lower-chroma preference, adjustable warm skin-tone protection, OKLab lightness retention and deterministic analytical gamut compression.
- Added linear-light White Balance Temperature/Tint plus a neutral-colour canvas sampler that calculates a bounded correction from a chosen grey or white point.
- Added three-way Colour Balance for Shadows, Midtones and Highlights with Cyan–Red, Magenta–Green and Yellow–Blue axes and optional Preserve Luminosity.
- Added native tiled WGSL implementations and startup CPU/GPU parity validation for all four selective-colour adjustments. Shader failure remains feature-scoped and falls back to the exact CPU reference.
- Preserved straight Alpha and hidden RGB beneath zero Alpha in RGBA8 and exact RGBA64 processing, including masks, Isolated/Pass Through groups, multi-document residency and atomic Undo/Redo.
- Advanced typed adjustment payloads to schema 3 with schema-1/schema-2 and legacy version-6 migration. Public `.vfxphoto` remains version 6 and private residency snapshots remain schema 7.

## 0.8.0b.1

- Fixed the Curves editor build failure by making `CurveCanvas::moveSelected` use the same by-value `CurvePoint` signature in its declaration and definition. The function intentionally mutates a local clamped point before storing it.
- No project-format, adjustment-schema, renderer, GPU, histogram or behaviour changes from 0.8.0b.

## 0.8.0b

- Added a professional Curves adjustment layer with RGB and per-channel editing, exact input histograms, point insertion/removal/dragging, numeric coordinates, keyboard nudging, smooth monotone and linear interpolation, channel reset and atomic Undo/Redo interactions.
- Added a shared tonal-mapping core for Levels and Curves. RGBA8 uses a precombined 256-entry GPU lookup texture; exact RGBA64 uses all 65,536 source codes on the CPU reference path.
- Added a globally bounded 16 MiB / 32-entry tonal lookup cache so large 16-bit documents reuse immutable mappings across tiles and documents.
- Expanded Exposure with linear-light Offset and Gamma controls while retaining Exposure EV.
- Reworked Contrast around adjustable linear luminance and a user-controlled tonal pivot to reduce colour shifts.
- Reworked Saturation using OKLab chroma scaling with gamut-boundary compression and exact identity handling.
- Advanced typed adjustment payloads to schema 2 with deterministic schema-1 and legacy version-6 migration. Public `.vfxphoto` remains version 6; private residency snapshots remain schema 7.
- Extended native tiled WGSL adjustment processing and startup parity coverage to Curves and the upgraded Exposure, Contrast and Saturation parameters.
- Preserved straight Alpha and hidden RGB beneath zero Alpha on both CPU and native GPU adjustment paths.
- Added regression coverage for Curves persistence, exact 8/16-bit mappings, hidden RGB, typed parameter migration and upgraded adjustment identities.

## 0.8.0a

- Replaced the basic Levels inspector with a professional histogram editor supporting RGB, Red, Green and Blue channels; input black/midpoint/white; output black/white; direct handles; numeric controls; linear/log display; Auto Levels; clipping feedback; Document/Selection scope; and black/grey/white sampling.
- Added a reusable renderer-input histogram service with exact compositing-position capture, Isolated/Pass Through group correctness, RGBA8 256-bin and RGBA64 65,536-bin analysis, transparent-pixel handling, cancellation, stale-result rejection and a shared bounded cache.
- Added schema-versioned typed adjustment payloads while retaining legacy scalar compatibility. Existing version-6 projects migrate automatically and new projects remain public format version 6.
- Advanced the private Hot/Warm/Cold snapshot schema to 7 so complete per-channel Levels data survives residency transitions; older private snapshots remain migratable or safely rebuildable.
- Extended CPU and native tiled WGSL Levels processing with per-channel input/output ranges while preserving straight Alpha, hidden RGB beneath zero Alpha, masks, selections, groups and exact 16-bit CPU fallback.
- Added focused regression coverage for typed/legacy persistence, per-channel/output-range processing, exact 8/16-bit histogram bins, sparse selection weighting, cancellation and adjustment-input capture across Isolated and Pass Through groups.

## 0.7.0e.5.1

- Split native GPU approval into independent foundation, compositor, raster/mask-brush and Clone Stamp capabilities. A compositor-only parity miss now routes only deeply mixed nested hierarchies through the CPU compositor without disabling validated standard compositing, GPU resize, adjustments, transform presentation, brush or Clone Stamp paths.
- Kept the strict deep mixed-hierarchy parity warning visible instead of hiding or globally relaxing it.
- Removed Qt 6 `QTransform::inverted()` nodiscard compiler warnings by using `isInvertible()` where the inverse matrix is not required.
- Replaced the oversized About dialog changelog wall with a compact milestone and architecture summary that fits on normal desktop screens.
- Avoided the KDE portal app-ID registration warning for uninstalled `run.sh` development builds while preserving the desktop ID for installed, Flatpak and AppImage launches.
- Public `.vfxphoto` format remains version 6.

## 0.7.0e.5

- Completes **Transform Expansion — Integration and Hardening** for Free Transform, Scale, Rotate, Skew, Distort and Perspective. Warp remains intentionally deferred.
- Makes whole-layer Apply all-or-nothing: the complete layer tree is copied, target/parent identity is revalidated, local transforms are calculated, raster and owner-mask storage is preflighted and baked in detached state, and only a fully valid result replaces the live tree.
- Makes selected-pixel, direct-channel and direct-mask transforms all-or-nothing across every selected target. Clipboard-transform preparation now distinguishes a valid off-target no-op from inversion/allocation failure, preventing cleared pixels or partially updated targets from being published.
- Adds shared transform-safety validation for finite/invertible matrices, projective horizons, ±1-billion document coordinates, 32768-pixel persistent extents, exact Qt snapshot limits and aggregate preparation memory. Unsafe numeric edits and pasted/cached recipes keep the last valid pending state and create no history.
- Preserves straight RGBA8/RGBA64 hidden RGB beneath zero Alpha, owner masks, sparse selections and bounded off-canvas origins through the hardened preparation paths.
- Advances the per-document render serial and retires session-qualified backend state before transformed surfaces are republished, so stale CPU/GPU work cannot overwrite Apply, Undo/Redo or a restored resident document.
- Validates transforms and storage origins during version-6 project save/load and private Hot/Warm/Cold cache serialisation. Invalid legacy project transforms are reset with a warning; unsafe private cache metadata is rejected.
- Hardens automatic legacy raster-transform baking before painting and from the Inspector with the same detached preflight/commit path.
- Removes a duplicate Flip Horizontal context-menu entry and a duplicate Crop state comparison.
- Adds regression coverage for bounded negative/off-canvas storage, projective-horizon and memory rejection, safe no-op versus failed paste, RGBA8/RGBA64 hidden-RGB transforms, invalid JSON repair, exact repeated image rotations and transform-recipe survival through Cold residency.
- Keeps Graphite as default and public `.vfxphoto` project format version 6.

## 0.7.0e.4.1

- Fixed Transform Again and Repeat Transform progressively squashing non-square content after 90° rotations. Affine layer recipes now replay their original document-space linear transform around the new pivot instead of multiplying X and Y independently by the target's swapped bounds.
- Split remembered workflow recipes into editable layer transforms and exact orthogonal whole-document operations. Image → Transform rotations and flips now repeat through the same lossless layered document transaction rather than being reconstructed as a layer-bounds matrix.
- Transform Again intentionally repeats fixed whole-image rotations/flips immediately because a canvas-size-changing image operation cannot be reopened as a meaningful editable layer session. Repeat Transform uses the same exact path.
- Updated transform clipboard JSON to version 2 with explicit recipe kind while retaining import support for version-1 layer recipes. Transform and Duplicate remains limited to layer recipes.
- Added four-quarter-turn regression coverage verifying alternating canvas dimensions, exact raster preservation, original layer transform restoration and resolution parity. Public `.vfxphoto` remains version 6.

## 0.7.0e.4

- Added Image → Transform commands for whole-document Flip Horizontal, Flip Vertical, Rotate 90° Clockwise/Counter-clockwise and Rotate 180°. Operations preserve the layered hierarchy, masks, selections, guides, PPI metadata, off-canvas storage, straight RGBA and hidden RGB, and create one atomic Undo entry.
- Added per-document Transform Again, Repeat Transform and Transform and Duplicate workflow recipes with `Ctrl+Shift+T`, `Ctrl+Shift+R` and `Ctrl+Alt+Shift+T` shortcuts. Recipes adapt the previous source bounds and pivot to the current target and remain isolated between open documents and residency states.
- Added copy/paste transform values through a versioned private MIME/JSON payload, available from Edit → Transform, the top Commands menu and the transform context menu.
- Added Nearest, Bilinear, Bicubic and Lanczos 3 transform interpolation. High-quality Apply paths sample straight RGB and Alpha independently and retain exact 16-bit component storage; masks and selected-pixel/channel transforms use the same chosen method.
- Added exact orthogonal-image, guide/selection/resolution and interpolation regression coverage. Public `.vfxphoto` remains version 6; Warp remains deferred.

## 0.7.0e.3.7

- Deferred Warp and removed it from the Transform mode dropdown and context menu.
- Rolled back the experimental Warp mesh, preview-worker and post-Apply session changes that caused unstable layer movement.
- Restored the tested 0.7.0e.2.1 transform implementation as the active foundation.
- Preserved Free Transform, Scale, Rotate, Skew, Distort and Perspective, including white idle/orange pending state and interior double-click Apply.
- Preserved atomic Undo/Redo, masks, selections, multi-layer transforms, hidden RGB, off-canvas storage, CPU/exact 16-bit fallbacks and public `.vfxphoto` format version 6.
- Marked Warp as deferred for a future redesign; the next planned stage is 0.7.0e.4 — Image Rotation, Repeat and Workflow.

## 0.7.0e.2.1

- Changes the transform outline, handles, rotation control and pivot to white whenever the Transform tool has no unapplied geometric change. The complete overlay switches to the existing orange accent only while the session contains a non-identity pending transform or an unapplied Alt-drag duplicate.
- Derives the visual state from the accumulated document-space transform rather than the broader session-active flag, so click-only sessions and transforms returned exactly to their starting matrix become white again.
- Adds double-click Apply inside the transform quadrilateral when changes are pending. Handles, the pivot, rotation control and clicks outside the quadrilateral cannot trigger the shortcut accidentally.
- Keeps the top-bar Apply/Cancel buttons, Enter/Keypad Enter and Escape behaviour unchanged, with Apply still producing one atomic Undo operation and Cancel remaining history-free.
- Adds canvas regression coverage for idle/pending accent colours and pending-only interior double-click routing. Public `.vfxphoto` remains version 6.

## 0.7.0e.2

- Adds **Skew**, **Distort** and **Perspective** to the persistent top-bar Mode dropdown and the right-click Transform menu without resetting the active transform session.
- Adds mode-specific canvas interaction: Skew drags one edge along its current tangent, Distort moves one corner independently, and Perspective couples the two adjacent control points while retaining the diagonal anchor. Handles, hit targets and cursors change with the selected mode.
- Adds numeric deformation controls. Skew edits the selected Top/Right/Bottom/Left edge midpoint; Distort and Perspective edit Top Left/Top Right/Bottom Right/Bottom Left control-point coordinates. Clicking a canvas handle selects the same numeric target.
- Represents every deformation as one document-space projective `QTransform`, preserving the shared multi-layer box, selected-pixel scope, direct channels/masks, owner-mask following, mode switching, snapping, one-step Apply/Undo and history-free Cancel.
- Rejects collapsed, crossed, non-finite and horizon-crossing quadrilaterals before they can replace the last valid pending transform.
- Adds a cached native WebGPU projective preview pipeline for eligible bounded 8-bit session surfaces. It inverse-maps the full 3×3 transform, bilinearly samples premultiplied foreground pixels and composites over the immutable renderer-prepared background; CPU presentation remains the fallback.
- Retains authoritative straight-component raster/mask baking, off-canvas storage extents, hidden RGB beneath zero Alpha, exact RGBA64 fallback, sparse selection inverse sampling, nested/Pass Through groups, residency and public `.vfxphoto` format version 6.
- Adds canvas interaction and projective-selection regression coverage plus an expanded Fedora integration test plan.

## 0.7.0e.1

- Adds the first **Transform Expansion** stage with a top-bar Mode dropdown for Free Transform, Scale and Rotate plus the same modes and useful commands in a right-click menu inside the shared transform box.
- Replaces mouse-release commit with a persistent transform session. Repeated move, scale and rotate gestures, mode changes, numeric edits, pivot moves, flips and quarter-turn commands compose without rebuilding the target transaction.
- Adds adaptive document-space X/Y, transformed W/H and Angle controls, an optional numeric proportions link, a movable pivot, Apply/Cancel buttons, Enter/Keypad Enter commit and Escape cancellation.
- Preserves the existing shared document-space multi-layer box, selected-pixel/channel/mask routing, Alt-drag duplication, guide/canvas/layer snapping and Ctrl snap bypass. Scale retains Shift uniform constraints and Rotate retains Shift 15-degree snapping.
- Keeps transform preview non-destructive and reuses one renderer-prepared background/foreground pair across the complete session. Eligible 8-bit hierarchy preparation uses the session-qualified tiled native renderer; CPU and exact 16-bit paths remain authoritative fallbacks.
- Commits the accumulated result once through existing structural history. Raster RGB and Alpha are transformed separately, masks follow their owners, and deformed raster/mask storage expands to preserve off-canvas content instead of clipping it to document dimensions.
- Adds canvas regression coverage for repeated gesture composition, pivot-only edits, pivot movement during scale, mode-filtered handles and transform-region context-menu routing. Public `.vfxphoto` remains version 6.

## 0.7.0d.4

- Completes the **Image Size and Resampling** integration/hardening stage without adding new resize modes.
- Adds an overflow-safe recursive preflight that validates resample method, finite layer transforms/reference origins, per-payload snapshot limits and the combined output footprint before any large CPU/GPU allocation. The UI derives a generous safe preparation budget from the shared residency target and current document footprint.
- Adds a post-build structural validator for exact canvas size/format/PPI, layer-tree identity, raster and mask output dimensions/formats, scaled reference extents/origins, conjugated transforms, sparse-selection size and finite in-bounds guides. A malformed provisional result cannot reach `replaceStructuralState`.
- Normalises colour space, device-pixel ratio and dots-per-metre metadata on accepted native GPU payloads so GPU and CPU outputs remain interchangeable through history, Cold snapshots and export.
- Checks cancellation immediately after a native accelerator returns, discarding a completed-but-cancelled payload before it can be counted or reused. Metadata-only preparation also gains cancellation checkpoints around its potentially shared state copies.
- Adds integration regression coverage for pre-allocation budget rejection, non-finite coordinate rejection, GPU metadata normalisation, cancellation after GPU return, empty layer trees and exact RGBA64/PPI/mask/selection/guide/off-canvas state through Hot/Warm/Cold eviction.
- Keeps atomic Undo/Redo, renderer/session stale-result rejection, all existing resampling methods and public `.vfxphoto` project format version 6.

## 0.7.0d.3

- Adds Catmull-Rom **Bicubic**, **Lanczos 3** and **Area / Box** methods to Image Size. Bicubic and Lanczos widen their separable kernels while reducing to suppress aliasing; Area integrates exact source-pixel overlap on reducing axes and uses Bilinear on expanding axes.
- Keeps Nearest Neighbour and Bilinear on the validated native tiled GPU path for eligible 8-bit payloads. Bicubic, Lanczos, Area, RGBA64 and sparse selections remain deterministic CPU-reference operations.
- Filters straight R, G, B and Alpha independently with explicit 8-bit/16-bit rounding and clamping, preserving hidden RGB beneath zero Alpha. Masks and selection coverage use the same method-specific scalar filters.
- Bounds high-quality provisional row storage to approximately 16 MiB by processing horizontal output blocks, preventing extreme one-axis reductions from retaining source-row intermediates across the full destination width.
- Extends Image Size units with Inches, Centimetres and Millimetres plus editable 1–9600 ppi resolution metadata. Pixel/Percent modes and linked or independent proportions remain available.
- Adds **Resample pixels** control. When disabled, the exact canvas, layers, masks, selection, guides, transforms, Alpha and hidden RGB remain byte-identical while only horizontal/vertical resolution and resulting physical print size change.
- Makes resolution part of the structural transaction, stale-result fingerprint and atomic Undo/Redo state. Resolution survives Hot/Warm/Cold eviction, version-6 project save/reopen and full-resolution export metadata.
- Adds regression coverage for exact Area averages, advanced-filter constant-component/hidden-RGB preservation, CPU-only dispatch, metadata-only pixel identity and rendered export resolution metadata. Public `.vfxphoto` remains version 6.

## 0.7.0d.2

- Adds native WebGPU Nearest Neighbour and Bilinear Image Size kernels for eligible 8-bit canvas, Raster/Base Image and finite mask payloads. Each editable payload remains independent; the document is never flattened.
- Splits destination work into bounded 256×256 tiles and uploads only the contiguous source patch required by each tile's global pixel-centre mapping. Bilinear patches include the exact neighbouring samples needed at tile boundaries, preventing 256-pixel seams.
- Filters straight R, G, B and Alpha independently in `rgba8unorm`, preserving hidden RGB beneath zero Alpha. Grayscale masks are packed and recovered without colour conversion; RGBA64 remains on the exact 16-bit CPU reference.
- Keeps GPU output provisional per payload. Allocation, shader, dispatch, readback or validation failure discards that payload and reruns it completely through the 0.7.0d.1 CPU reference, so a partially resized payload cannot publish.
- Serialises the one-off resize dispatch against shared renderer work, bounds temporary source/output/readback resources, checks cancellation between tiles, and retains the existing document-session stale-result and atomic structural publication boundary.
- Extends startup validation with CPU/GPU Nearest and Bilinear parity across output-tile boundaries, including zero-Alpha hidden RGB. Adds synthetic accelerator/fallback unit coverage and an optional real-GPU cross-tile parity test.
- Sparse document selections remain on the existing memory-bounded CPU reference path, and public `.vfxphoto` format version 6 plus Hot/Warm/Cold persistence remain unchanged.

## 0.7.0d.1

- Adds **Image → Image Size…** with the standard `Ctrl+Alt+I` shortcut, absolute pixel dimensions, percentage sizing and an optional linked aspect ratio.
- Adds alpha-safe CPU reference resampling with Bilinear and Nearest Neighbour methods. Straight R, G, B and Alpha are filtered independently in RGBA8 and RGBA64, preserving hidden RGB beneath zero Alpha.
- Resamples every editable Raster/Base payload and finite raster/group/adjustment mask independently instead of flattening the document. Nested groups, adjustment layers, visibility, opacity, blending and Pass Through structure remain editable.
- Scales raster/mask reference extents and origins together with every hierarchy coordinate system. Local transforms are conjugated into the resized document axes, so translations and nested world positions scale without leaving a global raster scale that would distort later brush coordinates; off-canvas storage remains preserved rather than clipped.
- Resamples sparse selection coverage in memory-bounded strips, scales feathering and guides, and keeps inactive/full states compact.
- Publishes the complete canvas, layers, masks, selection and guides through the existing all-or-nothing structural boundary with cancellable asynchronous preparation, exact stale-result checks, renderer retirement and one atomic Undo/Redo command.
- Keeps resolution metadata unchanged in this foundation stage, so resizing pixels changes the physical print size. Physical-unit/PPI controls and resampling-disabled metadata changes are reserved for 0.7.0d.3.
- Preserves public `.vfxphoto` format version 6 and Hot/Warm/Cold session snapshots. Adds core coverage for linked document-state scaling, bilinear hidden RGB, exact 16-bit components, cancellation and version-6 save/reopen.

## 0.7.0c.5

- Completes the **Canvas Size and Document Bounds** integration/hardening stage without adding new user-facing commands. Canvas Size, Reveal All, Fit, automatic Trim and Crop now publish through one all-or-nothing `PhotoDocument::replaceStructuralState` boundary. Candidate canvas, layer IDs, sparse selection and guides are fully validated before any live document member changes.
- Replaces revision-only selection checks with exact sparse-selection fingerprints and also verifies edit target, channel view and complete pending Crop state before accepting asynchronous bounds results.
- Makes structural Undo/Redo retire the renderer identity on every successful state application, including same-size destructive clipping. Old CPU tiles, GPU readbacks, previews and thumbnails cannot publish after history navigation merely because the width and height stayed unchanged.
- Refreshes selection presentation, session summaries, document-strip metadata and thumbnails after structural Undo/Redo, keeping multi-document and Hot/Warm/Cold UI state synchronised.
- Applies the completed-progress-dialog cancellation fix to Crop as well as the Canvas bounds command family, preserving genuine late cancellation while preventing programmatic close from mutating the cancellation token.
- Rejects canvas surfaces whose exact straight-RGBA payload cannot fit the private Hot/Warm/Cold snapshot image ceiling before allocation, and saturates structural-history byte/tile counters instead of allowing signed overflow.
- Adds regression coverage for atomic rejection of duplicate IDs, mismatched selections and invalid guides; pre-allocation rejection of unpersistable surfaces; and exact 16-bit Canvas Extension, selection, guide and hidden-RGB survival through Cold eviction/restoration. Public `.vfxphoto` remains version 6.

## 0.7.0c.4

- Adds **Image → Trim Transparent Pixels…**. The current visible merged composite is analysed and the canvas is fitted to every pixel whose final Alpha is greater than zero; hidden layers do not block trimming, while masks, opacity, transforms, adjustments and Isolated/Pass Through group behaviour remain authoritative.
- Adds **Image → Trim by Corner Colour…** with Top-left/Top-right/Bottom-left/Bottom-right sampling, inclusive 0–255 per-channel straight-RGBA tolerance and independent Top/Bottom/Left/Right side controls. Fully transparent samples match regardless of hidden RGB.
- Leaves entirely transparent or uniformly matching documents unchanged rather than collapsing them to 1×1. Results that already match the canvas also create no history entry.
- Analyses the composite in cancellable adaptively bounded strips (up to 256 rows), avoiding a second full-document allocation. Transparent trim reads authoritative premultiplied Alpha directly; corner-colour trim uses a new high-precision straight-RGBA region renderer. RGBA64 Alpha values as low as 1 remain significant.
- Preserves off-canvas raster/mask storage by default. Both dialogs provide optional **Delete pixels outside the trimmed canvas**, which reuses the exact destructive Canvas Size/Crop clipping path and remains one Undoable transaction.
- Reuses session/renderer/layer/canvas/selection/guide stale checks, renderer-serial invalidation, translated selections/guides, Hot/Warm/Cold snapshots and public `.vfxphoto` format version 6.
- Adds core regression coverage for hidden-layer exclusion, Pass Through group masks, RGBA64 Alpha=1, transparent hidden-RGB equivalence, exact tolerance boundaries, selected-side retention, destructive hidden-RGB clipping and cancellation.

## 0.7.0c.3

- Adds **Image → Reveal All**. It scans every Raster/Base Image payload in hidden and visible nested hierarchies, expands only as needed, never shrinks, never creates fill pixels and ignores masks as independent expansion sources.
- Reveal All treats any non-zero straight RGBA component as stored content, so RGBA8/RGBA64 hidden RGB beneath zero Alpha is revealed exactly; transparent-black allocation alone does not affect bounds.
- Adds **Fit Canvas to Selection**, using the exact bounding rectangle of all non-zero sparse selection coverage, including feathered fringe pixels. Selection coverage, guides and layer coordinates are translated and clipped through the existing bounds transaction.
- Adds **Fit Canvas to Selected Layers**. Hidden explicitly selected rasters count; groups union descendant bounds; enabled finite masks constrain raster/group bounds; adjustment layers contribute only when they have finite effective mask coverage.
- Aligns transformed floating-point layer bounds outward to integer pixels so rotations/scales are fully contained, and recognises one-step 16-bit Alpha values without 8-bit quantisation.
- Reuses the asynchronous cancellable Canvas Size publication boundary, session/renderer/layer/canvas/selection/guide stale checks, renderer-serial invalidation and one atomic Undo/Redo command. No-op and no-finite-bounds results create no history entry.
- Keeps public `.vfxphoto` format version 6 and existing Hot/Warm/Cold persistence. Adds core regression coverage for hidden-RGB Reveal All, never-shrink behaviour, exact selection fitting, hidden nested transforms, finite adjustment masks and unbounded-adjustment no-ops.

## 0.7.0c.2

- Expands **Image → Canvas Size…** with Transparent, Foreground Colour, Background Colour and Custom Colour fill choices. Foreground and Background resolve from the shared primary/secondary colour panel at dialog open; Custom supports Alpha.
- Creates a normal editable top-level **Canvas Extension** raster layer at the bottom of the layer tree when a colour fill is requested and the resize genuinely exposes new canvas. The former canvas area remains untouched, including mixed expand/shrink operations.
- Keeps Transparent as a pure bounds change with no invented raster payload and restores both Transparent and non-destructive clipping as the safe defaults whenever the dialog opens.
- Adds **Delete pixels outside new canvas**, off by default. Destructive Canvas Size reuses the proven Crop bake/clip path to remove raster and mask storage outside the final bounds while preserving exact in-bounds straight RGBA, Alpha and hidden RGB for translation-only 8-bit and 16-bit cases.
- Normalises a fully clipped selection to inactive, translates/clips guides, advances renderer identity and records fill-layer creation or destructive deletion in the existing single atomic Canvas Size Undo/Redo transaction.
- Preserves public `.vfxphoto` project format version 6; Canvas Extension is an ordinary Raster layer and destructive results use existing raster/mask fields, so Hot/Warm/Cold snapshots and save/reopen need no schema change.
- Adds regression coverage for mixed-axis extension fill, partially transparent colour, zero-Alpha hidden RGB in RGBA64, grayscale fill conversion, pure-contraction no-layer behaviour, 8/16-bit destructive clipping, same-size compaction of preserved storage and version-6 round-trip persistence.

## 0.7.0c.1.1

- Fixes Canvas Size silently doing nothing after **Resize Canvas** was pressed. The completed worker result was being discarded because programmatically closing `QProgressDialog` emitted its cancellation signal.
- Snapshots genuine user cancellation before completion cleanup and blocks progress-dialog signals during programmatic closure, so valid absolute/relative changes now commit for every anchor while real cancellation remains safe.
- Adds a focused Fedora regression check covering visible dimension changes, status feedback and Undo/Redo after the asynchronous commit.

## 0.7.0c.1

- Adds **Image → Canvas Size…** with absolute and relative width/height controls, a persistent nine-point anchor grid and the standard `Ctrl+Alt+C` shortcut.
- Implements deterministic anchor arithmetic. Centred odd differences place the unmatched pixel on the right or bottom for both expansion and contraction.
- Changes document bounds without scaling or rewriting stored raster/mask pixels. Transparent expansion is a pure bounds change; non-destructive contraction retains off-canvas straight-RGBA storage, Alpha and hidden RGB exactly.
- Translates complete layer trees through one root document map while preserving child transforms, raster/mask reference sizes and reference origins.
- Translates and clips sparse selections into the new document extent. A fully clipped result becomes an inactive selection rather than an invisible active-empty selection.
- Translates guides and removes guides outside the resized canvas.
- Publishes Canvas Size through one cancellable asynchronous transaction with session UUID, renderer serial, layer tree, canvas identity, selection revision and guide stale-result checks.
- Records the complete canvas, layer, selection, guide and pending-Crop state as one Undo/Redo operation. Canvas-size Undo/Redo now advances the per-document renderer serial before replacement previews are requested.
- Preserves public `.vfxphoto` project format version 6 and existing Hot/Warm/Cold persistence through the already stored canvas dimensions, transforms, reference extents, selections and guides.
- Adds regression coverage for all nine anchors, odd expansion/contraction, pure 8-bit and 16-bit storage preservation, hidden RGB, selection/guide clipping, cancellation and version-6 save/reopen.

## 0.7.0b.6.2

- Make horizontal slider grooves clearly visible in Midnight, Graphite and Daylight by using the suite theme border colour instead of the near-background scrollbar track colour.
- Match VFX Texture Lab slider geometry with a 4 px groove and 13 px accent handle.
- Add accent hover and readable disabled slider states.

## 0.7.0b.6.1

- Fixes the Shared Suite Themes build failure in `refreshThemeDependentUi()` by using the existing `PhotoDocument::hasImage()` API rather than the nonexistent `isLoaded()` method.
- No theme appearance, document, Crop, Colour-panel or project-format behaviour changes.

## 0.7.0b.6

- Replaces Photo Lab's former purple-only stylesheet with the VFX Texture Lab suite palettes: Midnight, Graphite and Daylight.
- Makes Graphite the default theme for first launch and invalid or missing theme settings.
- Adds a persistent, immediately applied View → Theme submenu.
- Themes Qt widgets, menus, docks, tabs, inputs, buttons, selections, scroll bars, progress bars and tooltips from shared colour tokens.
- Themes custom-painted canvas void/checkerboard/rulers, document-strip cards, layer thumbnails, colour swatches and toolbar/layer-control icons.
- Re-tints monochrome resource icons when switching themes so Daylight remains legible without restarting.
- Keeps image pixels, saved swatch colours, channel identity colours and functional canvas overlays unchanged.

## 0.7.0b.5

- Removed the redundant **Saved Swatches** heading plus **Save Current** and **Clear** buttons. Swatches continue to use the existing right-click Save/Replace/Clear menu, recovering enough vertical room for the primary Colour tab.
- Removed the Colour tab's internal `QScrollArea`; Colour, RGB and HSV now use direct stack pages with no tab-specific scroll bars. The wheel remains square and uses the available dock space between the compact header and two-row swatch grid.
- Retained the shared swatches, active-colour handling, Hex field, Eyedropper and all right-click swatch functionality without changing colour persistence or project data.

## 0.7.0b.4

- Rebuilt the Colour dock around a shared compact header: overlapping square primary/secondary swatches now carry embedded swap and black/white reset controls, while the Eyedropper and active-colour Hex field use the recovered horizontal space. Clicking either swatch makes it active and brings it to the front.
- Moved Saved Swatches outside the tab stack so the same swatch grid, Save Current and Clear controls remain available in Colour, RGB and HSV without duplicated state.
- Kept only the wheel/triangle, RGB controls or HSV controls tab-specific, giving the wheel more usable vertical room and removing the repeated empty header area from the numeric tabs.
- Hex editing now uses copy-friendly `#RRGGBB` or `#RRGGBBAA` notation for the active primary/secondary colour, including Alpha in the final two digits when it is not fully opaque.

## 0.7.0b.3

- Fixed rectangle, ellipse, freehand and polygonal selection gestures drawn wholly in the overscroll void creating an invisible active-empty selection that blocked Brush/Eraser and retouch editing. Selection gestures may still begin outside the image and cross into it, but a final result with no covered document pixels is now normalised to an ordinary inactive selection.
- Empty results produced by Replace, Subtract or Intersect gestures now appear as a normal **Deselect** history action, while Add/Subtract gestures wholly outside the document leave an existing selection unchanged.

## 0.7.0b.2

- Fixed the post-crop RGB Composite canvas remaining transparent even though direct Red, Green, Blue and Alpha channel views still contained valid pixels. Crop now publishes its newly advanced per-document render serial to `RenderBackend` before replacement composite tiles are scheduled, invalidating old work without causing all new native/CPU composite requests to be rejected as stale.
- Added the crop-to-composite renderer identity hand-off to the Fedora regression checklist.

## 0.7.0b.1

- Fixed the Fedora/GCC build failure in `MainWindow::layerCoverageSource()`. The Crop storage-reference calculation now correctly branches on the helper's `maskSource` argument instead of referring to an out-of-scope `target` variable.
- No project-format, rendering, Crop behaviour or persistence changes. Public `.vfxphoto` remains format version 6.

## 0.7.0b

- Replaced the Crop placeholder with a persistent per-document Crop tool. The frame can be created outside the image, moved or resized with eight fixed-screen handles, repositioned with Space while drawing, resized around its centre with Alt, ratio-locked with Shift and nudged by one or ten pixels with the arrow keys. Enter or double-click applies; Escape cancels the pending frame.
- Added Free, Ratio and Fixed Size modes; Original, 1:1, 3:2, 4:3, 5:4 and 16:9 presets; orientation swap; signed X/Y and exact W/H controls; adjustable outside dimming; snapping; and Rule of Thirds, Grid, Diagonal, Triangle, Golden Ratio and Golden Spiral overlays. O cycles overlays and Shift+O changes orientation.
- Added Crop from Selection and a Straighten line sampler with Shift-for-vertical behaviour plus numeric corrective angle preview. Selection coverage is transformed into the result. Ordinary crop guides are translated and clipped; angled guides are discarded because the current guide model is horizontal/vertical only.
- Made ordinary cropping genuinely non-destructive by separating document bounds from raster and mask storage rectangles. Off-canvas pixels remain in straight-RGBA storage and can be exposed by later canvas operations; existing raster content is no longer stretched when the document dimensions change. Crops extending beyond the left or top edge now pad storage with an explicit negative local origin so newly exposed transparent pixels are immediately editable.
- Added optional Delete Cropped Pixels. Axis-aligned destructive crops copy exact straight bytes, including hidden RGB under zero Alpha. Transformed 8-bit and 16-bit raster paths transform opaque RGB and Alpha separately, while masks, nested groups, adjustment/group masks and layer transforms are baked consistently without flattening the hierarchy.
- Added one atomic, cancellable Crop/Crop and Straighten history transaction with session UUID/serial, layer tree, selection, guide and canvas stale-result checks. Failed or cancelled work publishes nothing, and Hot/Warm/Cold private snapshots now preserve pending crop state plus explicit raster/mask reference extents.
- Updated painting, channels, masks, Clone/Healing/Spot/Patch, Dodge/Burn/Sponge, Blur/Sharpen/Smudge, mask overlays, layer bounds, CPU compositing and tiled GPU cache hashing to respect preserved off-canvas storage after a non-destructive crop. Selection snapshots remain document-sized even when raster storage is larger, so selection-aware Brush, Mask and native Clone Stamp paths sample the two coordinate spaces independently.
- Kept public `.vfxphoto` at format version 6. New optional `rasterReferenceSize`, `rasterReferenceOrigin`, `maskReferenceSize` and `maskReferenceOrigin` metadata preserve storage coordinates; projects without those fields retain the legacy full-document interpretation. Private Hot/Warm/Cold snapshots advance to format 6 for the origin metadata.

## 0.7.0a

- Replaced the live special-case Base Image node with an ordinary editable Raster layer for both new and imported documents. Blank documents start with **Background**; opened images retain the familiar **Base Image — filename** name while using normal raster behaviour.
- The initial raster can now be painted, erased, retouched, renamed, reordered, transformed, masked, grouped, duplicated and deleted through the same paths as every other Raster layer. Empty layer trees are valid and render as transparency.
- Added lossless legacy promotion for project versions 1–6 and private Hot/Warm/Cold session snapshots. Legacy Base Image UUIDs, names, masks, transforms, blend state, 8/16-bit pixels, alpha and hidden RGB are retained; source-backed legacy nodes receive an implicitly shared exact raster payload before becoming Raster nodes.
- Retained the previous large-image preview efficiency by substituting the existing preview mip into source-shared Raster snapshots, rather than repeatedly sampling the full-resolution initial layer for coarse canvas tiles.
- Kept public `.vfxphoto` at version 6 and added the optional `editableRasterBase` root marker so modern zero-BaseImage layer trees remain distinguishable from damaged older files. An untouched initial raster records `sourceRasterLayerId` and reuses the already embedded source PNG instead of duplicating a large payload; edited rasters continue to save their own exact pixels. Save validation now requires unique valid UUIDs and encodable raster/mask payloads rather than exactly one special Base Image.
- Removed Base Image-only duplication, removal, grouping and layer-tree restrictions while retaining the historical `baseLayerId()` API as a default-selection helper for the bottom-most raster.

## 0.6.0i.1

- Added named, per-case native GPU parity reporting for hierarchy masks, nested Pass Through groups, cross-boundary composites, mask Brush, straight-RGBA Clone Stamp, low-opacity overlapping Clone Stamp and scalar mask Clone Stamp. Terminal output now shows each case's measured delta and limit rather than one ambiguous combined maximum.
- Kept compositor, mask and hierarchy validation at the existing maximum delta of 2. The three Clone Stamp cases now use an explicit maximum delta of 3 to account for the bounded double-precision CPU versus f32 WGSL rounding seen after bilinear sampling, repeated soft coverage and one final RGBA8 quantisation; a larger result still disables native document work.
- Added explicit `RenderBackend::shutdownGpuFoundation()` and `WebGpuContext::shutdown()` paths. The normal GUI process drains native submissions/callbacks, releases resident textures and pipelines while Qt is still alive, and nulls handles so shutdown is idempotent.
- The disposable GPU helper now flushes its stable result and exits with `_Exit` without running in-process native/Qt/function-static teardown. Its graphics objects are reclaimed by the operating system at the process boundary, avoiding the late teardown path that emitted `corrupted double-linked list` after an otherwise complete diagnostic.
- The normal GUI process also explicitly releases its shared GPU foundation after the event loop exits rather than relying only on function-static destruction.
- Locked mapped GPU readback images to RGBA8888 and added a row-size allocation guard, preventing future scalar callers from copying four-byte GPU texels into a one-byte host image.
- Removed the unused `ImageProcessor::prepareImage()` helper and its `-Wunused-function` build warning.
- Public `.vfxphoto` remains format version 6.

## 0.6.0i

- Grouped Clone Stamp, Healing Brush, Spot Healing and Patch into one compact Retouch toolbar family. Right-click chooses a member and the most recently selected action remains visible as the family default.
- Reworked `ToolFamilyButton` painting to suppress every native Qt menu indicator before drawing the single custom corner triangle. Switching selection, retouch, tone or detail family members no longer leaves a second centred/downward arrow over the icon.
- Added cooperative cancellation for Healing Brush, Spot Healing and Patch operations. Escape while the canvas is focused cancels an active pointer gesture or pending candidate search, sparse active-region construction and Poisson relaxation without publishing partial pixels, selection movement or Undo history.
- Added explicit cancellation propagation through `HealingBrushRequest`, `SpotHealingRequest`, `PatchToolRequest` and the shared paint-commit result. Cancelled operations clear live/treatment/selection previews and retain the untouched document.
- Added cancellation checks throughout large candidate, coverage, neighbour, solver and writeback loops rather than only before dispatch.
- Added a 1.5-million-active-pixel guard to Healing Brush, matching Patch Tool's bounded sparse seamless solver and preventing pathological brush gestures from exhausting memory.
- Retained immutable source/destination snapshots, straight RGBA and hidden-RGB handling, exact destination Alpha for healing/patch operations, 8/16-bit CPU references, native tiled Clone Stamp parity/fallback, selection clipping, sparse history and stale-result publication guards.
- Added regression coverage proving pre-cancelled Healing, Spot Healing and Patch operations return no image, affected rectangle or error payload that could be mistaken for a publishable result.
- Public `.vfxphoto` remains format version 6; no retouch source, pickup, preview or cancellation state is persisted.

## 0.6.0h

- Added Smudge Brush as the third member of the compact Detail tool family, with persistent Size, Strength, Hardness and optional Finger Painting controls.
- Implemented an ordered within-stroke CPU reference: every new dab samples an immutable pre-dab neighbourhood immediately upstream of motion, then publishes before the next dab so picked-up material continues through the gesture without scan-order dependence.
- Raster Smudge transports straight RGBA, including Alpha and meaningful hidden RGB beneath zero Alpha. Alpha-aware bilinear sampling prevents transparent neighbours from contaminating visible colour while preserving hidden colour when the transported result remains transparent.
- Added Grey, Red, Green, Blue and Alpha channel targets plus raster/adjustment/group masks as scalar smudge surfaces.
- Active selection coverage is applied inside every ordered dab, preventing out-of-selection writes from becoming later stroke feedback; transformed targets remain evaluated in layer space.
- Live preview retains one persistent working image and appends only the newest pointer segment, keeping long gestures stable rather than replaying the complete stroke on every event. The authoritative full-resolution release uses the same ordered segment stream.
- Added RGBA8/RGBA64, Finger Painting, selection, channel, mask and incremental-versus-whole-stroke regression coverage.
- Existing native WebGPU Brush/Eraser, Clone Stamp, adjustment and compositor paths are unchanged; Smudge is CPU-authoritative in this stage.
- Public `.vfxphoto` remains format version 6; Smudge settings are application preferences and no stroke state is persisted.

## 0.6.0g

- Added Blur and Sharpen brushes in a compact Detail tool family alongside the planned Smudge tool.
- Added independent Size, Strength, Hardness and Radius controls; Sharpen also provides Protect Highlights.
- Blur uses two separable box cycles for a smooth Gaussian-style neighbourhood response with bounded O(pixel-count) work independent of radius.
- Raster neighbourhoods are alpha-aware: visible RGB is filtered in associated form, fully transparent neighbourhoods retain hidden RGB, and destination Alpha remains bit-identical in RGBA8 and RGBA64.
- Sharpen uses bounded unsharp masking with highlight attenuation to reduce clipping and bright-edge halos.
- Grey, R/G/B/Alpha channels and layer masks are supported as scalar targets. Sponge remains raster-RGB-only.
- Detail brushes reuse sparse 128×128 floating-point stroke coverage, immutable stroke-start pixels, document-space selection clipping, transformed target handling and one sparse 256×256 Undo command.
- Incremental live preview processes only newly increased stroke coverage and matches the authoritative whole-gesture result.
- Added regression coverage for Alpha preservation, hidden-RGB edge behaviour, scalar channel/mask filtering and incremental/whole-stroke parity.
- Existing native WebGPU Brush/Eraser, Clone Stamp, adjustments and compositor paths are unchanged; Blur and Sharpen are CPU-authoritative in this stage.
- Public `.vfxphoto` project format remains version 6.

## 0.6.0f.1

- Fixed Dodge, Burn and Sponge live previews becoming progressively slower during long mouse-down gestures. The old path replayed every previously collected segment on each pointer event, making preview work grow quadratically with stroke length.
- Added a sparse 128×128 floating-point tone-stroke coverage accumulator. Each pointer event now appends only the new segment while retaining the same maximum whole-gesture coverage semantics as the authoritative final commit.
- Recomputes changed preview pixels from the immutable stroke-start image, preserving event-rate independence, stable soft edges, tone-range behaviour and exact whole-gesture output without repeated 8-bit processing.
- Drops the renderer-only live-layer QImage reference after each synchronous preview render so the next event does not detach and copy the complete preview frame.
- Added incremental-versus-whole-gesture parity coverage over a long 180-segment stroke and bounded per-update dirty-region checks.
- Public `.vfxphoto` project format remains version 6.

## 0.6.0f

- Added a compact Dodge/Burn/Sponge toolbar family with dedicated icons and persistent per-tool options.
- Dodge and Burn provide Size, Exposure, Hardness, Shadows/Midtones/Highlights range targeting and optional Protect Tones.
- Added perceptual linear-light luminance adjustment that limits clipping and largely preserves hue while lightening or darkening the selected tonal range.
- Dodge/Burn support raster RGB, Grey/R/G/B/Alpha channels and raster/adjustment/group masks; Sponge deliberately supports raster RGB only.
- Sponge provides Saturate/Desaturate, Size, Flow, Hardness and optional Vibrance Protection while approximately preserving luminance.
- Tone strokes derive one stable floating-point coverage field from the complete gesture, avoiding event-rate-dependent over-processing and repeated 8-bit quantisation.
- Raster tone operations preserve straight Alpha bit-for-bit in RGBA8/RGBA64 and continue to edit hidden RGB beneath zero Alpha.
- Reused selection-aware preview/commit clipping, transformed-target coordinates, sparse 256×256 history, one-command Undo/Redo and document-session/layer/selection/transform stale-result rejection.
- Added regression coverage for tonal ranges, component-channel and mask targeting, saturation/chroma changes, luminance stability, explicit linear-sRGB handling and exact 16-bit Alpha preservation.
- Tone tools are CPU-authoritative in 0.6.0f; existing native Brush/Eraser, Clone Stamp and compositor paths are unchanged. Public `.vfxphoto` project format remains version 6.

## 0.6.0e

- Added a selection-driven Patch Tool with Source and Destination workflows.
- Source mode drags a sample beneath the fixed selected destination; Destination mode drags selected source material to a new destination and moves the selection with it.
- Added live selection-shaped drag previews for Current Layer and immutable rendered Composite sources.
- Reused the sparse Poisson seamless-cloning solver for boundary-matched colour and illumination while preserving destination Alpha and hidden RGB.
- Active selection feathering controls patch coverage; sources are not clipped by the selection.
- Patch release runs asynchronously with document-session, layer-revision, transform and selection-revision stale-result rejection.
- Pixels and Destination-mode selection movement are grouped into one Undo/Redo command, including selection-only movement over uniform image content.
- Added 8/16-bit CPU coverage, transformed coordinate handling, a dedicated toolbar icon, persisted Mode/Source/Opacity options and regression tests.
- Public `.vfxphoto` project format remains version 6.

## 0.6.0d.1

- Added a presentation-only 50%-red Spot Healing treatment overlay that follows brush size, hardness and active selection coverage while the gesture is being painted, remains visible during asynchronous processing, and disappears only when the healed result or an error is published.
- Kept the overlay out of compositing, thumbnails, clipboard data, exports, Undo history and `.vfxphoto` persistence.
- Added a full-document Select All copy fast path that preserves exact straight RGBA/hidden RGB through Qt implicit sharing instead of transform-resampling every pixel.
- Added an in-process clipboard cache keyed by a private clipboard token, avoiding private-payload serialisation and decoding for Copy → Paste within VFX Photo Lab.
- Deferred exact private MIME serialisation until an external process requests it while retaining normal system image interoperability.
- Avoided an additional full-document raster allocation/copy when the clipboard already covers the complete destination document.
- Added snapshot-backed Undo/Redo for full-frame pasted-layer creation, avoiding synchronous XOR generation, hashing and compression of every 256×256 tile. Smaller pasted fragments retain sparse tiled history.
- Added regression coverage for exact full-document Select All copy/materialisation.
- Public `.vfxphoto` project format remains version 6.

## 0.6.0d

- Added a dedicated Spot Healing toolbar tool with persistent Current Layer/Composite, Size, Opacity and Hardness controls.
- Added deterministic nearby-source search over staggered concentric candidate rings. Candidate footprints that overlap the complete blemish gesture are rejected before scoring.
- Candidate ranking compares boundary colour after removing a constant lighting offset, luminance gradients, texture energy, Alpha compatibility and distance. Invalid/out-of-bounds patches are rejected.
- Reused the 0.6.0c.1 sparse Poisson seamless-cloning solver for the selected patch, preserving destination Alpha exactly and retaining straight hidden RGB beneath zero Alpha.
- Spot Healing supports raster RGB targets, active selections, transformed document mapping, Current Layer and immutable rendered Composite sources, sparse 256x256 raster history and asynchronous stale-result rejection.
- Spot Healing intentionally publishes after mouse release rather than changing candidate patches during the gesture.
- Added deterministic source-selection, blemish-removal and exact Alpha-preservation regression coverage.
- Public `.vfxphoto` project format remains version 6.

## 0.6.0c.1

- Replaced the conservative two-scale Healing Brush kernel with sparse Poisson seamless cloning, following the same core formulation documented and implemented by GIMP's GPLv3 Healing tool and based on Todor Georgiev's seamless-cloning method.
- The sampled source now supplies the interior image gradients and structure. A red/black Gauss–Seidel solver with successive over-relaxation computes a harmonic destination-minus-source correction from the brush boundary, so hard destination seams and blemish structure are replaced rather than merely blurred.
- Solves the complete accumulated stroke mask in perceptual straight RGB while keeping memory sparse through 256x256 coverage tiles and active-pixel neighbour tables. Long or diagonal strokes do not allocate their full rectangular bounding box.
- Preserved immutable source/destination snapshots, transformed source mapping, Aligned/non-aligned behaviour, Current Layer/Composite sampling, exact destination Alpha, hidden RGB repair, selection clipping, sparse history and stale-result rejection.
- Added regression coverage proving a high-contrast destination step is converted into a smooth boundary-matched transition rather than a faint source imprint over the original edge.
- Healing remains CPU-authoritative in this quality pass. Public `.vfxphoto` project format remains version 6.

## 0.6.0c

- Added Healing Brush as the next Retouching Tools stage, with its own toolbar action, bandage icon, persistent Size/Opacity/Hardness/Source/Aligned preferences and independent transient source anchor for every open document.
- Reused the proven Clone Stamp interaction contract: Alt-click source sampling, Aligned and non-aligned document-space offsets, immutable Current Layer or rendered Composite source snapshots, transformed-layer coordinate mapping and presentation-only source crosshairs.
- Added an authoritative straight-RGBA8/RGBA64 CPU healing kernel. A bounded two-scale analysis transfers sampled high-frequency texture while replacing source colour and illumination with local destination statistics.
- Healing edits raster RGB only and preserves destination Alpha bit-for-bit, including hidden RGB updates beneath zero Alpha. Channels and masks deliberately remain Clone Stamp targets rather than receiving arbitrary healing semantics.
- Rebuilt live Healing preview from the immutable stroke-start raster and complete gesture, then commits asynchronously through existing document-session, layer-revision, selection-revision and world-transform stale-result guards.
- Added selection-aware healing, sparse 256x256 raster history and one Undo command per completed stroke. Active-empty selections, invalid source/target changes and out-of-document source reads create no edit.
- Added 8-bit colour-adaptation, zero/partial-alpha hidden-RGB and exact 16-bit Alpha regression coverage. Public `.vfxphoto` project format remains version 6; Healing source anchors are intentionally transient.

## 0.6.0b.1

- Fixed soft, low-opacity Clone Stamp strokes developing red/green or rainbow-like contour bands where many dabs overlap. CPU and WGSL paths now accumulate the complete stroke as floating-point transmittance and quantise each edited pixel only once.
- Preserved the established per-dab opacity build-up mathematically with `1 - product(1 - dab coverage)` while removing channel-specific 8-bit threshold accumulation. Raster, channels, masks, straight alpha and hidden RGB retain their existing semantics.
- Rebuilt the CPU-authored live Clone preview from its immutable stroke-start image and the complete gesture, so separate mouse-move events cannot re-quantise the same soft pixels repeatedly.
- Extended startup and core parity coverage with 5% opacity, 0% hardness and strongly overlapping dabs. The native GPU path still falls back transaction-wide to the CPU reference on any failure.
- Public `.vfxphoto` project format remains version 6.

## 0.6.0b

- Added the native tiled WebGPU Clone Stamp commit path for eligible 8-bit raster, Grey/component-channel and mask targets. Existing live preview behaviour remains CPU-authored while the authoritative full-resolution stroke now enters the shared asynchronous renderer transaction.
- Added immutable per-destination-tile source patches with one-pixel bilinear halos. Affine target/source transforms are evaluated in WGSL, and source coordinates outside the complete source image contribute no paint rather than clamping or wrapping.
- Ported the 0.6.0a.2 alpha-aware source filtering, straight-RGBA blend and feathered-selection clipping semantics to WGSL, including hidden RGB preservation beneath zero Alpha and halo-free soft cloning onto transparency.
- Made Clone Stamp tile publication transaction-wide: all GPU dispatches complete before any document image is assembled, stale or failed generations are cancelled, partially synchronized cache surfaces are invalidated, and the complete stroke is rerun through the CPU reference.
- Kept RGBA64/Grayscale16 sources, projective transforms, unavailable native devices and guarded oversized source footprints or provisional-tile transaction budgets on the authoritative CPU path. No partial GPU result is accepted as a mixed stroke.
- Extended startup native parity validation across four 256×256 destination tiles, fractional source coordinates, transparent hidden RGB, feathered selection coverage and mask cloning. Added optional native Clone parity tests plus a CPU-fallback integration test.
- Added `shaders/clone_stamp.wgsl` as the readable source mirror for the embedded native kernel. Public `.vfxphoto` format remains version 6 and Clone source anchors remain transient per-document state.

## 0.6.0a.2

- Fixed soft Clone Stamp edges becoming dark or colour-shifted when cloning onto transparent raster pixels. Brush coverage now interpolates source and destination in associated-alpha space, then stores the result as straight RGBA.
- Made RGBA bilinear source sampling alpha-aware so transparent neighbouring texels cannot contaminate visible sampled colour. Fully transparent samples still retain and transfer meaningful hidden RGB.
- Applied the same alpha-aware interpolation to Clone Stamp live-preview selection clipping and authoritative feathered-selection clipping. Channel and mask scalar behaviour is unchanged.
- Recorded direct Base Image raster editing as a near-term workflow item after retouching rather than changing its semantics during the Clone foundation.
- Public `.vfxphoto` project format remains version 6.

## 0.6.0a.1

- Fixed GCC/Qt 6 compilation of `CloneStamp.cpp` by including the complete `QColorSpace` definition before passing `QImage::colorSpace()` to `QImage::setColorSpace()`.
- No Clone Stamp behaviour, rendering, history, selection, persistence or project-format semantics changed from 0.6.0a. Public `.vfxphoto` format remains version 6.

## 0.6.0a

- Added the first Retouching Tools stage: Clone Stamp with Alt-click source sampling, Aligned/non-aligned behaviour, Current Layer/Mask/Channel and rendered Composite sources.
- Added transient per-document clone anchors and aligned offsets, presentation-only source/cursor crosshairs and persistent tool Size, Opacity, Hardness, Source and Aligned preferences.
- Added an authoritative CPU clone kernel with bilinear source sampling, transformed-layer coordinate mapping, source-edge no-op behaviour, straight RGBA8/RGBA64 blending and hidden-RGB transfer beneath zero alpha.
- Added selection-aware cloning into raster pixels, Grey/R/G/B/Alpha channels and raster/adjustment/group masks, reusing existing sparse tile history and target-specific Undo/Redo.
- Composite cloning captures the hierarchy before a stroke and before automatic `Clone Layer` creation, preventing recursive self-sampling. Current-target cloning also uses an immutable pre-stroke image.
- Added document-session, target revision, selection revision and world-transform publication checks to clone commits.
- Added CPU regression coverage for hidden RGB, component channels, masks, affine source mapping, out-of-bounds sampling and 16-bit transfer.
- Public `.vfxphoto` project format remains version 6; clone source state is intentionally transient.

## 0.5.0h.2

- Allowed Rectangle, Ellipse, Freehand Lasso and Polygonal Lasso gestures to begin anywhere in the canvas viewport, including the grey overscroll void. Unclamped document-space paths are clipped only when committed, while a tiny plain click in the void still performs the established Deselect command.
- Added canvas regression coverage for void-start marquee, freehand and polygonal gestures. Public `.vfxphoto` format remains version 6.

## 0.5.0h.1

- Fixed GCC 16 compilation of the CPU tiled raster-brush path by explicitly converting Qt `QColor::*F()` float values to `double` before applying `std::clamp` with double bounds.
- No document, rendering, selection, history or `.vfxphoto` format behaviour changed from 0.5.0h. Public project format remains version 6.

## 0.5.0h

- Fixed direct Grey/R/G/B channel presentation so it renders the selected Base/Raster layer's own stored colour values instead of allowing transparent hidden RGB from sibling paint layers to overwrite the view. Alpha presentation continues to use the selected layer's real alpha.
- Fixed the Tool Options toolbar to retain one stable height across Selection, Transform, Brush and other tools.
- Moved eligible 8-bit raster and mask selection clipping into the tiled brush engine and native WebGPU kernel. Selection coverage is sampled in document space per dirty tile and applied to the completed stroke before publication.
- Reworked CPU raster stamping to operate directly in straight RGBA8/RGBA64, preserving hidden RGB during erasing without depending on premultiplied QPainter intermediates.
- Added CPU/GPU parity coverage for hard and feathered selection-aware raster and mask strokes, including overlapping stamps and hidden RGB.
- Hardened asynchronous channel preview publication with document-session, render-serial, selected-layer UUID and layer-revision validation.
- Hardened project and private-session selection tile-count validation against integer overflow while retaining public `.vfxphoto` format version 6.

## 0.5.0f.1

- Fixed Transform entering Whole Layer(s) after a normal pixel selection. Entering Transform now automatically chooses Selected Pixels whenever the active document selection and edit target are usable; selection changes made while Transform is already active update the automatic scope as well. A manually chosen Scope remains authoritative for that Transform-tool session.
- Alt+drag Move now duplicates eligible selected layer/group roots, selects the duplicates and continues the same live move gesture. The duplicate and movement commit as one structural Undo step; Base Image and adjustment layers retain their existing restrictions.
- Repaired direct R/G/B/Grey/Alpha Copy, Cut and Paste routing by resolving the visible Channels view against the current Layers target instead of a stale retained edit-target layer. Direct paste also reaffirms the resolved edit target after commit.
- Added Ctrl-click thumbnail selection loading without changing Layers selection: pixel thumbnails load stored layer alpha and mask thumbnails load stored soft mask coverage, including gradients and mask inversion, as a Replace selection.
- Restored consistent Delete routing after canvas transforms: Delete removes the selected layer whenever the canvas is using a non-selection tool, or when a selection tool has no non-empty active selection. Delete still clears selected contents only while a selection tool owns a non-empty selection, and text controls retain ordinary Delete behaviour.
- Passed transform mouse-down modifiers directly from `ImageCanvas`, making Alt-drag duplication deterministic rather than relying on global modifier polling.
- Added offscreen regression coverage for Ctrl-click thumbnail routing and preserved tree selection.
- Public `.vfxphoto` project format remains version 6.

## 0.5.0f

- Added a Transform scope switch between **Whole Layer(s)** and **Selected Pixels**.
- Selected Pixels extracts active selection coverage from one or more Raster/Background layers, clears the source alpha-safely, and commits move/resize/rotate into the existing layer pixels without changing the layer transform.
- Direct Grey/R/G/B/Alpha channel and layer-mask transforms are supported when that target is active.
- The document selection is transformed by the same document-space affine matrix and Undo/Redo restores pixels, target state and selection together.
- Existing document, guide, visible-layer/group-bound snapping and Ctrl bypass apply to selected-pixel transforms.
- Groups and adjustment layers remain available through Whole Layer(s) scope and are intentionally rejected by Selected Pixels.
- Fixed selected-region raster Paste being interpreted as a full-document raster and enlarged to the canvas. Clipboard fragments are now materialised into full-document storage before layer insertion.
- Fixed channel and mask Copy/Cut/Paste routing by treating the session edit-target layer as authoritative, including when the Channels panel owns focus or the Layers tree has a different selection.
- Copy, Cut and Paste shortcuts now use application-level routing while retaining normal behaviour inside text editors.
- Added core regression coverage for cropped-paste scale, exact channel transfer, transformed raster pixels and selection-following movement.
- Public `.vfxphoto` project format remains version 6.

## 0.5.0g

- Added Copy, Cut, Paste and Copy Merged for raster pixels, direct R/G/B/Grey/Alpha channels and raster/adjustment/group masks.
- Added a versioned private VFX Photo Lab clipboard MIME payload preserving straight RGBA8/RGBA64, greyscale values, independent application coverage, colour space, source document bounds and original placement.
- Added normal system image clipboard data alongside the private payload for interoperability with other applications.
- Internal raster pastes retain original document coordinates; external images are centred. Pasted pixel content becomes an ordinary raster layer immediately, with no floating-selection state.
- RGBA pasted into channels or masks converts to luminance and uses clipboard Alpha as application coverage. Greyscale payloads preserve their copied values and soft selection coverage independently.
- Destination selections continue to constrain direct channel and mask pastes. The selection itself remains unchanged after every paste.
- Cut uses the same alpha-safe target-specific clear semantics as 0.5.0e and supports the whole applicable target when no selection is active.
- Pasted layers and direct-target pastes use compressed sparse tile history and one Undo step. Raster tile history now retains exact straight/premultiplied 8-bit formats or RGBA64 precision, so hidden RGB and 16-bit clipboard data survive Undo/Redo as well as the initial paste.
- Public `.vfxphoto` project format remains version 6.

## 0.5.0e

Selection-Aware Painting, Masks and Clear.

- Multiplied Brush/Eraser coverage by the active 8-bit document selection for raster/Base pixels, Grey/R/G/B/Alpha channel targets, and masks on raster, adjustment and group layers.
- Kept live previews selection-clipped while preserving full-resolution final quality; eligible native GPU stamping is followed by a bounded CPU selection-coverage commit pass, and stale results also validate selection revision and layer world transform.
- Preserved straight alpha-safe storage when selection-clipped raster painting or erasing reaches zero alpha, retaining hidden RGB exactly.
- Blocked active-empty selection strokes before paint-target creation, preventing empty history commands and accidental new layers.
- Added Edit → Clear Selected Contents plus focus-aware Delete routing: canvas Delete clears the active target through the selection, Layers Delete removes layers, and text controls keep normal Delete behaviour.
- Made Clear target-specific: Raster/Base and Alpha reduce only alpha, R/G/B reduce only the chosen component, Grey reduces RGB without touching alpha, and masks clear stored coverage toward black.
- Added sparse raster, component, Grey and one-byte mask Undo for Clear, including transformed document-space selection sampling and 8/16-bit reference handling.
- Initialised newly added masks from the active selection, automatically attached a selection-derived mask to new adjustment layers, and added Create/Replace Mask from Selection for selected raster, adjustment or group layers. No-selection mask creation remains compact white.
- Kept the current selection active after painting, clearing and mask creation; public `.vfxphoto` remains version 6 and Hot/Warm/Cold session isolation is unchanged.

## 0.5.0d

- Added Select-menu sources for the current raster/Background layer’s stored alpha and the current layer’s stored mask, each with Replace, Add, Subtract and Intersect modes.
- Source coverage is mapped through the complete layer/group world transform into document space. Layer opacity and masks do not alter stored pixel alpha; mask inversion is honoured even while a mask is disabled.
- Added Feather, Expand, Contract and Smooth selection commands with document-pixel radii and one sparse Undo entry per successful operation.
- Added a tiled edge-processing core that evaluates independent 256×256 output tiles with bounded source halos, retaining allocation-free active-empty and Select All polarity where possible.
- Added Refine Selection with live presentation-only red-overlay and marching-ants preview for Smooth, Feather, Contrast and Shift Edge. Cancel leaves the document untouched; OK performs one full-resolution commit.
- Added arbitrary soft-coverage combination tests, morphology/feather seam tests, sparse full-selection feather checks, complete contraction and refine edge-shift direction coverage.
- Public `.vfxphoto` format remains version 6. Native WebGPU compositing, CPU/16-bit fallback, straight alpha-safe storage and Hot/Warm/Cold document isolation are unchanged.

## 0.5.0c

Selections and Local Editing, stage 3: Freehand and Polygonal Lasso.

- Added Freehand Lasso and Polygonal Lasso to the compact right-click selection-tool family, with canvas-scoped `L` and `Shift+L` shortcuts and dedicated toolbar icons.
- Freehand input is sampled and simplified in screen space, keeping point density independent of document zoom while preserving corners and continuously updating only the local vector-outline damage. Release closes the outline automatically.
- Polygonal Lasso provides a live final segment and first-vertex closure marker. Double-click, Enter or clicking the first vertex commits; Backspace removes the latest vertex; Escape cancels without touching document state.
- Unified rectangle, ellipse and lasso commits behind one `QPainterPath` selection transaction. Replace/Add/Subtract/Intersect, anti-aliasing, deferred bounded combined preview, active-empty semantics and exactly one sparse Undo command now behave identically across all four selection tools.
- Added tiled `SelectionMask::combinePath()` rasterisation with odd-even self-intersection handling, full/empty canonicalisation and unchanged public `.vfxphoto` version 6 persistence.
- Added core and canvas regression coverage for anti-aliased arbitrary paths, path combination/history, freehand point simplification, polygon vertex editing, Enter/Escape completion and first-vertex closure.
- Painting, masks, adjustments, transforms and deletion remain intentionally unrestricted until the later local-editing stages.

## 0.5.0b.1

Rectangle/Ellipse Select interaction polish.

- Removed full preview-mask rasterisation from the pointer-motion path. Continuous marquee dragging now repaints only the old/new vector-outline region; a bounded 512 px combined-selection preview is coalesced until the pointer pauses.
- Kept committed and transient selection contours separate in `ImageCanvas`, preventing stale preview ants while the marquee moves and avoiding any document/compositor mutation.
- Made a plain click in the grey canvas void request the same true Deselect command as a plain click over the image. Tiny image clicks now deactivate the selection rather than leaving an active-empty surrogate.
- Replaced the wide split-button arrow with a full-size tool button carrying a small bottom-right family marker. Left-click activates the retained shape; right-click opens the Rectangle/Ellipse family menu.
- Added canvas regression coverage for plain-click deselection both over the image and in the surrounding viewport.
- Public `.vfxphoto` remains format version 6; selection-restricted painting remains scheduled for 0.5.0e.

## 0.5.0b

Selections and Local Editing, stage 2: Rectangle, Ellipse and Selection Commands.

- Added a grouped Rectangle/Ellipse Select control to the left toolbar, with canvas-scoped `M` and `Shift+M` shortcuts and the most recently chosen shape retained as the button's primary action.
- Added Replace, Add, Subtract and Intersect controls to the top tool-options bar. Shift, Alt and Shift+Alt provide temporary mouse-down overrides without changing the stored mode.
- Added anti-aliased 8-bit rectangle and ellipse rasterisation directly into the sparse 256×256 `SelectionMask`, including correct full/empty canonicalisation and document-edge clipping.
- Added fixed 1:1 square/circle geometry and draw-from-centre options. Selection coordinates use document pixel edges rather than brush-centre coordinates, keeping marquees aligned with exact pixel boundaries.
- Added non-destructive live combined-selection preview. Dragging never changes document state; release constructs the full-resolution sparse result once and creates exactly one Undo command.
- Added Space repositioning for an unfinished marquee, Escape cancellation, outside-canvas dragging with document clipping, and an independent direct gesture outline so subtract/intersect gestures remain visible even when their resulting coverage is empty.
- Hardened selection history for implicit Select All to explicit partial-selection transitions where tile representation changes even when a tile's byte values remain 255. Undo/Redo now round-trips those representation-only changes exactly.
- Added core and canvas regression coverage for soft shape combination, all four combine modes, implicit-state history, fixed/from-centre geometry, Space repositioning and Escape cancellation.
- Public `.vfxphoto` remains format version 6. Painting, masks, adjustments, transforms and deletion are intentionally not selection-restricted in this stage.

## 0.5.0a

Selections and Local Editing, stage 1: Selection Core, History and Display.

- Added one persistent 8-bit document-space `SelectionMask` per `PhotoDocument`, independent of the active layer, channel or mask.
- Added sparse 256×256 selection tiles around an implicit coverage value. Select All and active-empty states require no full-image allocation.
- Distinguished no active selection from an active selection containing zero coverage, preventing future local tools from treating an empty intersection as unrestricted editing.
- Added Select All (`Ctrl+A`), Deselect (`Ctrl+D`), Invert Selection (`Ctrl+Shift+I`) and Show Selection Edges (`Ctrl+H`) commands.
- Added compressed symmetric XOR selection history with per-document statistics and exact Undo/Redo for sparse, soft and implicit selection states.
- Added a presentation-only animated marching-ants contour at the 50% coverage boundary. The contour is generated at preview resolution, cached between animation ticks and never enters image pixels, thumbnails, exports or compositor tiles.
- Extended private Hot/Warm/Cold session snapshots to retain selection coverage and per-document edge visibility, with integrity validation and backward reading of previous private snapshot versions.
- Added optional defensive selection persistence to public `.vfxphoto` format version 6. Older version-6 projects remain compatible; damaged selection data is discarded with a repair warning rather than risking document data.
- Added regression coverage for sparse implicit states, soft-tile history, version-6 round trips, damaged-selection repair, Cold snapshot restoration and presentation-only contour rendering.
- Rectangle, ellipse, lasso and polygonal selection tools remain intentionally deferred to 0.5.0b/0.5.0c. Painting and other edits are not selection-restricted until their later 0.5.0 stages.

## 0.4.3d.2

Transform snapping polish.

- Added a Transform Options **Snap** control, enabled by default, plus a configurable 1–128 px snapping distance. The distance is measured in screen pixels so the interaction remains consistent at every canvas zoom level, and both settings persist between runs.
- Moving one layer or a multi-layer selection now snaps the combined visible bounds by left/centre/right and top/centre/bottom anchors. This covers all four corners, four edge centres and the selection centre without changing the relative positions of selected layers.
- Snap targets include document edges and centres, visible user guides, and the visible bounds/centres of other layers. Unrelated groups contribute one composite target; when transforming a child inside a group, visible sibling branches remain available without duplicate parent targets.
- Resizing snaps only the dragged edge or corner while the opposite anchor remains fixed. Shift-constrained corner resizing remains uniform and honours the closest compatible snapped axis; rotation keeps its established 15-degree Shift snapping.
- Added per-axis hysteresis to prevent jitter between nearby targets, temporary amber alignment indicators across the canvas, and a Ctrl modifier that bypasses transform snapping for the current drag.
- Added canvas regression coverage for snapped movement, Ctrl bypass and dragged-edge resize snapping. Project format remains version 6; multi-document residency, masks, alpha-safe channels, Pass Through groups and CPU/GPU rendering are unchanged.

## 0.4.3d.1

- Fixed newly opened documents occasionally inheriting the previous canvas pan origin and appearing off-centre until revisited. New sessions now begin from a cleared presentation, enter Fit mode explicitly, and re-fit once after Qt finishes laying out the bottom strip.
- Fit-to-view is now durable per-document state rather than being restored as a fixed zoom. Warm and Cold document switches preserve whether a document was following the viewport or using a user-defined zoom/pan.
- Raised the lightweight Warm-session guard from four to twenty-four documents. The existing 1.5 GiB decoded-document/history budget remains authoritative, so ordinary sets of similarly sized photos no longer spill to disk merely because a fifth document was opened.
- Private session snapshots now preserve Fit-mode state with backward-compatible reading of the previous internal snapshot revision. Public `.vfxphoto` project format remains version 6.

## 0.4.3d

Multi-Document Workspace, stage 4: Bottom Document Strip and simultaneous document switching.

- Added a compact bottom document strip inside the central workspace, so the left Tools toolbar and right Layers/Channels docks continue all the way to the bottom as requested.
- Implemented the strip with one `QListView` model/delegate rather than one nested widget per document, keeping hundreds of thumbnails lightweight. Cards show a composite minimap, elided document name, unsaved marker, active highlight, close control, pixel dimensions and Hot/Warm/Cold residency tooltip.
- File → New, Open Image and Open Project now add sessions instead of replacing the current document. Image/project dialogs, multi-file canvas drops, command-line launch and the Linux desktop entry accept multiple files in one operation.
- Added safe active-session switching through the shared canvas, renderer and `QUndoGroup`, restoring each document's channel/edit target, selected layers, zoom, scroll position, guides, dirty state and accepted diagnostics.
- Warm sessions switch immediately; Cold sessions restore from the validated private `.vfxsession` cache before binding to the canvas. Failed restore leaves the previous document active and reselects its card.
- Added `Ctrl+Tab` / `Ctrl+Shift+Tab` navigation, Close Document, Close Other Documents and Close All Documents, plus matching card context-menu actions and per-document save/discard/cancel prompts. Closing an inactive document returns to the document that was active before its save prompt, including after a cancelled Save As.
- Added coalesced composite workspace thumbnails that never include the temporary red mask overlay or isolated channel presentation. Thumbnails survive Cold eviction and refresh after accepted edits.
- Re-enforce the shared residency budget after accepted document edits, allowing old Warm sessions to move Cold when the active document grows. CPU/GPU tile budgets remain process-wide at 256 MiB / 512 MiB.
- Added regression coverage for a 250-document strip model, horizontal scrolling, activation signals and workspace-thumbnail survival across Cold eviction/restoration.
- Public `.vfxphoto` remains format version 6; alpha-safe channels, masks, Pass Through groups, sparse history, native tiled rendering, CPU reference and 16-bit fallback are unchanged.

## 0.4.3c

Multi-Document Workspace, stage 3: Disk-Backed Document Residency.

- Added `DocumentResidencyManager` with process-wide Hot, Warm and Cold session states, least-recently-used eviction, a 1.5 GiB decoded-document/history target and up to four Warm sessions.
- Added a private per-run `SessionCacheStore`. Inactive documents are committed atomically through `QSaveFile`; a failed snapshot leaves the session resident and usable rather than risking data loss.
- Added a private binary `.vfxsession` codec separate from public `.vfxphoto` format 6. Exact image bytes are stored in independently compressed, SHA-256-checked 4 MiB chunks without PNG/base64 expansion.
- Snapshots preserve source pixels, Base/Raster overrides, recursive layers, masks, transforms, revisions, adjustments, group modes, guides, colour space/resolution, dirty baseline, channel/edit target, layer selection and viewport state.
- Restore validates the complete temporary document—pixel formats and dimensions, unique layer UUIDs, exactly one Base Image, finite transforms, tree depth/count and chunk integrity—before replacing any resident state.
- Normal Cold eviction preserves each session's Undo/Redo stack and clean index. Structural/property history now reports retained image memory alongside sparse raster/mask/channel deltas.
- Added a hard-pressure fallback that clears the oldest Cold Undo stack only when retained history alone still exceeds the global target. The exact disk checkpoint and dirty/save state remain intact.
- Cold eviction releases only that session's CPU/GPU renderer namespace; the existing 256 MiB tile RAM and 512 MiB VRAM budgets remain shared and unchanged.
- Added per-run cache cleanup on normal shutdown, live-process lock protection, seven-day cleanup for unlocked abandoned run directories and About diagnostics for residency counts, resident pixels, history, backing bytes, limits and cache location.
- Added regression coverage for exact 16-bit snapshot round-trips, damaged-cache non-replacement, LRU eviction/restoration, preserved Undo and hard-pressure history checkpointing.
- Kept the UI intentionally single-document for this foundation build. Project format remains version 6; masks, alpha-safe channels, Pass Through behaviour, CPU reference rendering and 16-bit fallback are unchanged.

## 0.4.3b.1

Mask Editing Overlay polish.

- Split layer-mask editing into two intentional presentation modes without changing mask data, compositing or history.
- Selecting a mask thumbnail in Layers now keeps the RGB/Grayscale composite visible and draws mask coverage as a presentation-only red overlay: black is transparent, white is 50%-opaque red and intermediate values scale linearly.
- Selecting Mask in Channels retains the established isolated black-and-white inspection/editing view.
- New masks enter the Layers-panel overlay mode instead of replacing the canvas with an all-white mask view.
- Added independent session state for the edit target and channel presentation, so structural Undo/Redo and future document switching preserve overlay versus isolated-mask intent.
- Live mask strokes update only the changed overlay region while the normal composite continues using the existing tiled CPU/GPU paths. The overlay never enters exports, thumbnails, project pixels or compositor caches.
- Mask-stroke Undo/Redo restores the presentation mode in which the stroke was created.
- Added canvas regression coverage for proportional overlay coverage, partial-region updates, clearing and unchanged backing pixels.
- Project format remains version 6; mask history, Pass Through behaviour, native tiled rendering and 16-bit fallback are unchanged.

## 0.4.3b

Multi-Document Workspace, stage 2: Session-Aware Renderer.

- Added explicit render-session contexts carrying both the stable document-session UUID and its current globally unique render serial.
- Namespaced every CPU tile-cache address and native GPU residency key by document session, preventing cross-document aliasing even when source cache keys, layer UUIDs, revisions, tile coordinates and domains are identical.
- Added session-scoped surface invalidation, complete session eviction and per-session resident-working-set statistics while preserving one process-wide 256 MiB RAM and 512 MiB VRAM budget.
- Replaced document replacement's global renderer clear with a scoped session reset. Future session activation can retain warm caches from other documents instead of flushing the complete workspace.
- Hardened asynchronous preview and paint work with both session UUID and render serial. The backend rejects obsolete contexts before work starts and after queue waits; the canvas/model acceptance gate repeats the same identity check.
- Stored accepted displayed-render diagnostics per session and restricted publication to the active session. Inactive or stale workers cannot overwrite About diagnostics.
- Added regression coverage for identical-surface cache isolation, scoped invalidation/session eviction, obsolete renderer rejection and active-session diagnostic restoration.
- Kept the visible UI deliberately single-document. Project format remains version 6; CPU reference, 16-bit fallback, masks and Pass Through behaviour are unchanged.

## 0.4.3a

Multi-Document Workspace, stage 1: Document Session Foundation.

- Added a dedicated `DocumentSession` owner for the active `PhotoDocument`, per-document Undo stack, dirty baseline, active channel/layer target, selected layer IDs, viewport state, layer-thumbnail cache and raster/mask/channel history statistics.
- Moved the document-state snapshots used by structural and property Undo into the session boundary.
- Replaced window-owned Undo/Redo actions with a `QUndoGroup`, allowing future document switching to activate a different session stack without recreating shortcuts or menu actions.
- Made asynchronous paint and preview rejection use each session's globally unique render serial; document replacement advances that serial atomically with session-local reset.
- Guarded Undo clean-state notifications by session identity so an inactive session can never mark the active document clean or dirty.
- Added session-owned selected-layer and canvas viewport snapshots, plus explicit canvas scroll-position capture/restore APIs for later switching.
- Kept the visible workflow intentionally single-document for this validation build. No project-format change: `.vfxphoto` remains version 6.
- Added regression coverage for independent session ownership, reset semantics, unique render serials and viewport-state round-tripping.

## 0.4.2.1

New Document dialog layout polish.

- Prevented the Width, Height and Resolution controls from being vertically compressed on first open under Linux desktop styles and high-DPI/font configurations.
- The dialog now derives its initial and minimum size from the completed layout while remaining resizable.
- Reserved the natural styled height and a practical minimum width for both pixel-dimension spin boxes.

## 0.4.2

New Document Workflow.

- Added File → New Document and Ctrl+N, plus a matching welcome-screen action.
- Added a focused creation dialog with document name, reusable screen/texture/print presets, custom width and height, orientation swap and print-resolution metadata.
- Added RGB and Grayscale document modes, 8-bit and 16-bit integer precision, sRGB and Linear sRGB working spaces, and White, Black, Transparent, Primary-colour or Custom-colour backgrounds.
- New documents start with one white paintable Background Base Image by default rather than an empty source or an extra automatically created Paint Layer.
- Added direct Layer Pixels painting on blank-document Background layers while retaining the established source-backed behaviour for opened images.
- Added grayscale-aware Channels UI: Grey and Alpha replace the RGB component set, grayscale Brush strokes write equal R/G/B values, and Grey-channel history preserves alpha.
- Persisted optional document name, colour model, working space, blank-document state and resolution metadata without changing project format version 6.
- Added document status reporting for colour model, bit depth, working profile and resolution, plus large-document memory estimates and warnings.
- Added regression coverage for white RGB creation, 16-bit grayscale project round-trips and invalid-dimension rejection.

## 0.4.1

Alpha-Safe Editable Channels.

- Made Red, Green, Blue and Alpha real editing targets for Base Image and Raster layers. Selecting a component in Channels shows it as greyscale; Brush writes primary-colour luminance and Eraser writes black without changing the other components.
- Added straight RGBA8/RGBA64 component painting. Alpha strokes update alpha alone, so hidden RGB remains intact even where alpha is zero. Sixteen-bit channel editing stays on the exact CPU tiled reference path.
- Added component-only sparse Undo/Redo: one byte per changed pixel for 8-bit channels and two bytes for 16-bit channels, with integrity hashes and affected-tile-only storage.
- Added optional Base Image pixel overrides, allowing direct channel edits without manufacturing a Paint Layer. Existing source-backed projects remain unchanged until the base image is edited.
- Added independent RGB channel reference rendering so hidden colour can be inspected while alpha is black. Normal composite, masks, Isolated/Pass Through semantics and native tiled presentation remain unchanged.
- Made flattened export alpha-safe. The normal composite supplies final alpha while fully transparent RGB is recovered from an independent straight-colour reference; the built-in TGA writer retains all four bytes exactly.
- Normalised imported and restored source/raster payloads to straight RGBA storage while retaining project format version 6 and backwards-compatible optional payload fields.
- Added regression coverage for zero-alpha TGA RGB, alpha-only strokes, all four component write masks, 16-bit preservation, component history and Base Image project round-trips.

## 0.4.0e.1

Layer and Mask Workflow milestone completion.

- Added document-session serials to asynchronous paint commits and preview tile batches. Results from a replaced, closed or reopened document are rejected even when layer UUIDs or revision numbers happen to match.
- Centralised document replacement cleanup: queued/running preview work is cancelled, accepted backend diagnostics are reset, tiled RAM/VRAM residency is cleared and raster/mask history statistics return to zero.
- Blocked structural, mask, property, transform, save/open/close and Undo/Redo mutations while a stroke commit, transform or export owns an authoritative document snapshot.
- Made flattened export cancellable during the full-resolution CPU reference render and changed PNG/JPEG/TIFF/WebP plus TGA writing to atomic replace-on-success output. A failed encoder cannot truncate an existing file.
- Hardened project saving by requiring exactly one Base Image, unique non-null UUIDs and successful PNG encoding for every embedded raster and mask before committing the project file.
- Extended recoverable project loading to unexpected raster dimensions as well as masks; repaired raster data is normalised to document space and reported through the existing repair workflow.
- Expanded startup native parity with semi-transparent raster and mask coverage crossing both 255/256 tile boundaries, in addition to adjustment, mask-brush and nested Pass Through validation.
- Consolidated regression coverage for combined full-resolution export, atomic output preservation, raster repair, displayed-backend reset and the historical layer/mask/Pass Through failures.
- Updated About, architecture, backend, roadmap and Fedora test documentation. Project format remains version 6 and sixteen-bit documents remain on the CPU reference path.

## 0.4.0d.3

Nested-group and workflow hardening.

- Propagated preview-generation cancellation from the asynchronous tile scheduler through `RenderBackend`, native hierarchy preparation and the CPU reference compositor. Obsolete work now stops before cache publication and cannot replace a newer generation.
- Added accepted-image diagnostics: About now identifies the backend path of the document tiles actually published to `ImageCanvas`, including visible Pass Through/Isolated group counts, maximum nesting depth, preview generation and mip level. The last backend operation is reported separately.
- Added native hierarchy resource preflights for visible node count, nesting depth and estimated temporary texture memory. Excessive or failed native allocations fall back to the CPU reference path instead of risking device or process instability.
- Hardened CPU Pass Through allocation and cancellation failure propagation so an incomplete before/after mix is discarded rather than published.
- Expanded startup native parity to a deeper mixed Pass Through → Pass Through → Isolated → Pass Through hierarchy with transformed masks, inversion and partial opacity.
- Added regression coverage for deep mixed-mode dirty-region/full-render agreement, project-format-6 save/load, recursive duplication and movement, cancelled tile publication, accepted-path diagnostics and resource-guard fallback.
- Kept visual compositing semantics, project format version 6, sparse raster/mask history and sixteen-bit CPU rendering unchanged.

## 0.4.0d.2.1

Native Pass Through parity and CPU fallback crash hotfix.

- Fixed the large-region CPU Pass Through mixer corrupting `QImage` memory when several QtConcurrent workers simultaneously triggered implicit detachment. The destination is now detached once on the calling thread and workers write through stable, disjoint row pointers.
- Corrected native varying-alpha parity validation to compare premultiplied RGBA, matching the CPU working canvas and the values that actually contribute to compositing. Straight RGB is undefined near zero alpha and had produced a false maximum difference of 63 despite the displayed premultiplied result differing by at most 2.
- Kept the varying-alpha diagnostic case, but it now validates visible/composited error rather than hidden low-alpha colour. Native Pass Through remains disabled if premultiplied error exceeds tolerance.
- Added a large 512×384 masked Pass Through regression that crosses the parallel-row threshold and renders twice to verify deterministic, crash-free CPU fallback behaviour.
- Kept native WGSL Pass Through semantics, project format version 6 and sixteen-bit CPU rendering unchanged.

## 0.4.0d.2

Layer and Mask Workflow — native tiled Pass Through groups.

- Added native WebGPU evaluation for Pass Through groups. Child stacks begin from the current parent accumulator rather than transparency.
- Added a dedicated WGSL before/after mixer and embedded reference shader that apply group opacity and masks in premultiplied RGBA8 space, matching the CPU reference semantics.
- Reworked native hierarchy encoding around immutable accumulator textures so nested Pass Through evaluation cannot overwrite the saved parent result.
- Preserved true isolation boundaries for Pass Through-inside-Isolated, Isolated-inside-Pass-Through and fully nested Pass Through combinations.
- Extended startup parity validation to masked, inverted, partially opaque and nested Pass Through cases. A failure keeps the complete canvas hierarchy on the CPU reference path.
- Updated About diagnostics to identify the native Pass Through path and combined hierarchy parity result.
- Kept 16-bit documents on the CPU reference path and project format at version 6.

## 0.4.0d.1

Layer and Mask Workflow — Pass Through groups, CPU reference stage.

- Added explicit **Isolated / Normal** and **Pass Through** compositing modes for groups. Existing and newly created groups default to Isolated, preserving every pre-d.1 project appearance.
- Implemented transform-aware CPU full-image and dirty-region Pass Through evaluation. Children begin from the current parent accumulator, allowing adjustment layers inside the group to affect layers below it.
- Made group opacity and masks interpolate between the parent result before entering the group and the result after evaluating its children. Disabled and inverted masks retain their established semantics.
- Preserved real nested boundaries: Pass Through cannot escape an isolated ancestor, and an isolated child remains contained inside a Pass Through parent.
- Added an Inspector mode selector, a `PT` group-thumbnail badge and explanatory state. Boundary blend-mode controls are disabled while Pass Through is active, while the stored blend mode is retained for switching back to Isolated.
- Added one-command Undo/Redo, recursive-duplication preservation and optional project-format-6 `compositingMode` persistence. Missing fields load as Isolated; invalid fields are repaired with the existing project-repair warning.
- Included group compositing mode in composite tile revision hashes. Documents containing Pass Through groups deliberately use the CPU tiled reference compositor in d.1; isolated documents retain the native WebGPU hierarchy path.
- Added regression coverage for parent adjustment reach, mask/opacity before-after mixing, nested isolation boundaries, full/region agreement, tiled CPU routing, duplication and save/load repair.

## 0.4.0c.2

Stage 3 UI and extreme-zoom performance hotfix.

- Fixed canvas repaint cost growing with the device-space size of the zoomed image. The transparency checkerboard is now generated only for the exposed viewport instead of iterating across the entire image rectangle; a 4096×4096 document at 3200% no longer attempts roughly 67 million checker cells per frame.
- Added an offscreen regression test that renders a 4096×4096 transparent document at 3200% and requires the repaint to remain viewport-bounded.
- Raised the Inspector dock's practical minimum width, disabled horizontal scrolling and made position/scale/rotation spin boxes shrink responsively. Mask controls can now appear or rebuild without pushing the Inspector contents sideways.
- Kept Stage 3 duplication, project-format-6 persistence, GPU tiled rendering, mask painting and sparse raster/mask history unchanged.

## 0.4.0c.1

Layer and Mask Workflow — Stage 3 structural and persistence completeness.

- Added recursive layer/group duplication from the Layers-panel button, Layer menu, context menu and `Ctrl+J`. Selected roots are duplicated directly above their originals, relative order is retained and one operation creates one structural Undo command.
- Regenerated every UUID in a duplicated subtree and remapped the active layer-pixel or mask target into the duplicate. Raster data, adjustment settings, masks, mask enabled/inverted state, transforms, opacity, blend mode and nested hierarchy are preserved.
- Hardened subtree insertion against conflicting or internally duplicated descendant UUIDs and prevented duplication of the embedded base image or a group containing it.
- Preserved mask pixels, state and world-space appearance through movement between transformed groups, recursive duplication, save/load and flattened rendering.
- Added bounded thumbnail caching keyed by layer UUID, revision and image identity. Active-target borders, disabled slashes and a dedicated inverted-mask badge remain independently cached and immediately correct after structural Undo/Redo.
- Made project-format-6 loading tolerant of damaged optional masks. Unsupported/corrupt mask payloads are discarded without losing the owning layer; unexpected mask formats and dimensions are converted/resized to document-space greyscale.
- Added an aggregated **Project Opened with Repairs** warning and marks repaired projects as modified so a clean copy can be saved.
- Added regression coverage for recursive UUID remapping, duplicate independence, insertion conflict rejection, masked hierarchy movement, duplicate project round-tripping, flattened render equality and malformed-mask recovery.
- Kept project format version 6, native tiled rendering, GPU mask painting, sparse raster/mask history and 16-bit CPU behaviour unchanged.

## 0.4.0b.2

- Removed the full-mask RGBA-to-greyscale conversion from every live pointer event; only the changed mask patch is now copied into a persistent greyscale preview surface.
- Live mask compositing uses the CPU dirty-region reference path to avoid synchronous GPU upload/readback overhead during pointer movement; the authoritative full-resolution stroke commit remains GPU-backed when eligible.
- Throttled live mask thumbnail resampling while preserving an exact final thumbnail after commit.
- Raster layer rotate and resize operations now bake the affine result into raster pixels and masks, then reset the layer transform so later Brush/Eraser stamps remain circular. Translation remains non-destructive.
- Legacy rotated/scaled raster layers are rasterised as a separate undoable command on the first subsequent Brush/Eraser stroke.
- Added a dynamic Mask entry to Channels for layers with masks, including transformed and inverted mask presentation and live updates while painting. Selecting it also selects the mask editing target; selecting layer pixels returns safely to RGB Composite.
- Starting a layer transform from the Mask channel now returns to RGB Composite before the transform preview, avoiding an ambiguous mask-only foreground/background preview.

## 0.4.0b.1

Layer and Mask Workflow — Stage 2 mask painting and sparse history.

- Routed Brush and Eraser to the selected mask on raster, adjustment and group layers without creating or modifying raster pixels.
- Brush writes the primary colour's luminance to greyscale mask pixels; Eraser restores white coverage. Disabled and inverted masks remain editable with their existing compositing semantics.
- Added live dirty-region mask compositing and live mask-thumbnail updates during a stroke, including off-canvas entry/exit and transformed hierarchy targets.
- Added a dedicated native WebGPU mask-stamping path using the existing deterministic WGSL brush kernel, separate mask tile addresses and exact greyscale conversion. Sixteen-bit documents remain on the CPU path.
- Added compressed, integrity-checked one-byte XOR mask tile deltas. One completed mask stroke is one Undo action, and undoing the first stroke restores the original compact 1×1 mask.
- Added separate raster and mask history diagnostics in About, plus startup CPU/GPU mask-brush parity gating.
- Added regression coverage for compact-mask restoration, one-byte storage, tile-boundary strokes, coverage restoration and GPU/CPU parity. Project format remains version 6.

## 0.4.0a.2

Layer and Mask Workflow — Stage 1 compositor/input hotfix.

- Replaced the native tiled mask-preparation path's indirect `createAlphaMask()` conversion with direct transformed greyscale coverage, preserving exact black/white values for compact, inverted and disabled masks.
- Added an isolated startup parity gate covering raster, adjustment and group masks with compact 1×1 masks. A mask mismatch now disables native canvas rendering and retains the CPU reference rather than allowing an incorrect GPU result.
- Cancelled rejected canvas paint gestures immediately when a mask is selected, so the brush cursor no longer freezes after the Stage 1 mask-painting guard blocks the stroke.
- Allowed Brush and Eraser gestures to begin in the overscroll area, cross the image boundary in either direction and continue tracking outside the document while committing only pixels that intersect the image.
- Kept mask painting itself for Stage 2, project format 6, 16-bit CPU behaviour and all existing sparse raster history semantics unchanged.
- Added compact-white inverted/disabled CPU mask regression coverage and extended the Fedora test checklist around native mask parity and off-canvas stroke entry.

## 0.4.0a.1

Layer and Mask Workflow — Stage 1.

- Added complete mask lifecycle controls for raster layers, adjustment layers and groups: create, remove, enable/disable and non-destructive inversion.
- Added separate layer-pixel and mask thumbnail columns in the Layers panel, with an explicit highlighted editing target.
- Added target selection through thumbnail clicks, the Layer menu, the layer context menu and Inspector controls.
- Added clear Inspector and status-bar reporting for Layer Pixels versus Layer Mask, including disabled and inverted state.
- Preserved the selected editing destination through tree rebuilds and structural Undo/Redo; adding a mask selects it and Undo/Redo restores the matching target.
- Added optional `maskEnabled` and `maskInverted` properties to project-format-6 layer JSON without changing the format version or breaking older projects.
- Updated CPU full-image and dirty-region compositing for disabled and inverted raster, adjustment and group masks.
- Updated native tiled hierarchy hashing and effective-mask preparation so the existing WebGPU compositor receives the same enabled/inverted coverage as the CPU reference.
- Kept 16-bit documents on the CPU path and retained the completed tiled raster/GPU architecture.
- Deliberately blocked Brush and Eraser while a mask is selected; mask painting and greyscale tile-delta history are reserved for Stage 2.
- Added regression coverage for lifecycle methods, CPU rendering on all supported mask-bearing layer types, save/load state and native tiled inverted-mask parity when WebGPU is available.

## 0.3.0e.1

Redo shortcut hotfix.

- Rebuilt the Redo shortcut list from Qt's platform-standard bindings and added both `Ctrl+Shift+Z` and `Ctrl+Y` as explicit portable fallbacks.
- De-duplicated identical key sequences before registering the action, preventing Qt from treating two copies of the same Linux standard binding as ambiguous.
- Set Redo to window shortcut scope so it remains available while the canvas or a side panel has keyboard focus.
- Added regression coverage requiring both common Redo conventions with no duplicate or empty sequences.
- Kept tile-delta history, project format 6, rendering, GPU paths and the 100-command history bound unchanged.

## 0.3.0e

Sparse raster-history milestone.

- Replaced Brush/Eraser document snapshots with compressed per-tile XOR deltas over only the affected 256×256 raster tiles.
- Made each tile payload symmetric so the same bytes perform Undo and Redo instead of retaining separate before/after tile copies.
- Preserved one completed stroke as one Undo command, including automatic Paint Layer creation/removal and exact layer placement on Redo.
- Restored null transparent raster layers exactly rather than materialising permanent full-document transparent images after Undo.
- Added explicit raster-surface invalidation on Undo/Redo so stale native residency cannot survive a history transition.
- Added per-tile before/after integrity hashes so a stale or misordered command is rejected without mutating unexpected pixels.
- Kept the existing 100-command deterministic history bound while substantially reducing ordinary stroke storage.
- Added live About diagnostics for raster-history command, tile and compressed-byte totals.
- Added exact round-trip, affected-tile-only, boundary, compression, state-rejection, null-raster and nested created-layer placement regression coverage.
- Kept project format version 6, CPU fallback, GPU brush/compositor paths and presentation scheduling unchanged.

## 0.3.0d.1

Native WGSL parity and atomic adjustment-publication hotfix.

- Matched `ImageProcessor`'s intermediate RGBA8 `lround()` step inside the WGSL adjustment pass before mask, opacity and blend-mode compositing. This removes the four-value accumulated parity error without weakening the existing maximum-delta gate.
- Expanded failed parity diagnostics with the maximum delta, differing-channel count and exact worst pixel/channel CPU and GPU values.
- Staged authoritative content-edit tiles offscreen and committed the complete visible level-0 generation in one validated canvas batch, eliminating centre-outward old/new adjustment mosaics.
- Kept the previous complete sharp viewport visible until the staged generation is complete; incomplete or cancelled generations publish nothing.
- Excluded the navigation prefetch border from atomic edit waits. Settled pan/zoom requests still populate it normally.
- Kept live Brush/Eraser updates, transform previews, progressive navigation, project format 6 and CPU/high-bit-depth fallback unchanged.
- Added an all-or-nothing canvas batch regression test.

## 0.3.0d

Native GPU adjustment-layer milestone.

- Added one WGSL compute pass for Exposure, Contrast, Saturation and Levels, matching the established CPU equations and per-pass 8-bit quantisation.
- Integrated adjustment operations directly into the recursive GPU hierarchy command stream, including nested groups, masks, opacity and every existing adjustment-layer blend mode.
- Removed adjustment-containing RGBA8 stacks from the blanket CPU-only exclusion; unsupported devices, failed shaders and high-bit-depth images still fall back per tile to `ImageProcessor`.
- Extended the isolated startup diagnostic so native document work is approved only after both the identity tile and a combined four-adjustment CPU/GPU parity test pass.
- Made helper approval depend on the diagnostic's explicit `PASS` result code rather than matching human-readable status text, so a partial-success/final-failure message can never enable native document work.
- Kept content-derived tile revisions, stale-publication rejection, direct canvas presentation, progressive navigation, project format 6 and one-command-per-stroke Undo unchanged.
- Added an installed `adjustment_tile.wgsl` reference shader and a native nested-group/mask parity regression test when WebGPU is available.

## 0.3.0c.1

Edit-presentation stability hotfix for the GPU Canvas Presentation checkpoint.

- Reserved coarse multiresolution tiles for actual zoom and pan interaction; paint, erase, visibility, opacity, hierarchy and transform commits now request authoritative level-0 tiles only.
- Prevented delayed settle callbacks created during content reconfiguration from replacing an authoritative edit plan with a coarse navigation plan.
- Stopped coarse tiles from being copied into the persistent backing image, so temporary navigation sampling cannot leave the document blurred for subsequent edits.
- Changed level-0 publication to update one continuous backing image and clip overlapping coarse records, avoiding fractional-scale dark seams between independently painted fine tiles.
- Promoted a completed live Brush/Eraser composite immediately, allowing rapid consecutive strokes to start from the latest visible pixels while asynchronous tile validation continues.
- Committed the final live transform presentation into the backing image before scheduling authoritative tiles, removing the brief snap back to the pre-transform position on mouse release.
- Added canvas regressions for edit backing isolation, partial fine/coarse coverage, consecutive-stroke handoff and transform-preview commit parity.
- No project-format, export, adjustment, brush-spacing or Undo-semantic changes from 0.3.0c.

## 0.3.0c

GPU Canvas Presentation and Progressive Zoom checkpoint.

- Replaced full-preview publication with revision-checked visible tile batches delivered directly to `ImageCanvas`.
- Added a four-level preview pyramid and viewport-aware coarse-level selection.
- Added centre-first scheduling, small asynchronous tile batches and a one-tile full-resolution prefetch border.
- Added a 140 ms interaction-settle phase: transient pan/zoom requests render only an appropriate coarse level, then settled requests refine progressively to level 0.
- Added independent content-generation and viewport-request serials so stale or cancelled work cannot replace newer tiles.
- Added shared cancellation tokens so obsolete pan, zoom, paint, transform and document requests stop after their current tile instead of draining stale batches.
- Added a bounded deterministic canvas presentation cache plus an incrementally updated backing image; no full composite image is rebuilt for every tile publication.
- Direct tile coverage clips out the backing image and coarser levels underneath sharper tiles, preserving transparent pixels and avoiding alpha accumulation during refinement.
- Preserved existing grayscale channel views, live brush presentation, single-stroke Undo, CPU fallback, project format and full-resolution export.
- Added regression tests for resolution-level cache separation, progressive coverage, CPU parity, stale canvas publications, transparent fine-over-coarse replacement and settled viewport notifications.

## 0.3.0b.1

Linux/GCC build hotfix for the Tiled Canvas Core checkpoint.

- Replaced the nested aggregate default argument on `TileCache` with explicit default and budget-taking constructors. This fixes GCC rejecting `Budgets budgets = {}` during compilation.
- Added a constructor/budget regression test covering both forms.
- Updated `QCryptographicHash::addData` calls to the Qt 6 `QByteArrayView` overload, removing the deprecation warnings shown by Fedora.
- No rendering, cache-budget or project-format behaviour changes from 0.3.0b.

## 0.3.0b

Tiled Canvas Core checkpoint.

- Added revisioned 256×256 raster and composite tile addressing.
- Added bounded RAM/VRAM caches with deterministic LRU eviction and dirty-tile protection.
- Added transactional tile publication, rollback and stale-result rejection.
- Moved Brush/Eraser commits to affected GPU-backed raster tiles with the CPU implementation retained as fallback.
- Preserved discrete brush spacing, hardness, opacity, seam-free cross-boundary painting and one Undo action per stroke.
- Added GPU hierarchy compositing for visibility, opacity, affine transforms, masks, blend modes and nested isolated groups.
- Kept adjustment-containing and high-bit-depth rendering on the CPU reference path.
- Added layer revisions and project format version 6 with backward loading for versions 1–5.
- Expanded backend diagnostics with cache and residency statistics.

## 0.3.0a.1

- Prevents native WebGPU/driver probing from blocking application startup.
- Shows the main window first, then runs the adapter/device/tile diagnostic in an isolated helper process.
- Stops the helper after 20 seconds if a native driver call stalls, while leaving the CPU renderer and application usable.
- Adds stage-by-stage terminal diagnostics to identify adapter, device, dispatch, or readback stalls.

## 0.3.0a

Native WebGPU foundation checkpoint.

- Added automatic, pinned `wgpu-native` v29.0.1.1 acquisition scripts for Linux and Windows. Normal build scripts fetch the matching SDK on first use; `VFXPHOTOLAB_SKIP_WGPU_FETCH=1` keeps an explicit CPU-only build path.
- Replaced the instance-only probe with real high-performance adapter, device and queue acquisition behind `WebGpuContext`. Missing runtimes, unsupported adapters and request failures remain non-fatal and preserve the CPU renderer.
- Added native RGBA8 texture allocation, padded texture upload, WGSL compute-pipeline creation, command encoding/submission, storage-texture output, asynchronous mapped-buffer readback and deterministic resource release.
- Added an isolated 64×64 identity-tile parity self-test during application startup. The About dialog and terminal report whether adapter/device/queue creation and GPU texture compute/readback matched the CPU reference.
- Added an automated GPU round-trip regression test which skips cleanly on CPU-only or unsupported systems.
- Kept all real document painting, compositing and settled rendering on the existing CPU path for this checkpoint. The GPU diagnostic cannot mutate project pixels.
- Added runtime library copying/RPATH handling for local Linux and Windows builds.

## 0.2.3

Transform interaction, Inspector cleanup and backend-status honesty.

- Replaced translation-only layer offsets with nondestructive affine transforms in full-resolution document coordinates. Project format version 5 stores translation, scale and rotation matrices while versions 1–4 remain loadable.
- Added a persistent transform boundary whenever the Transform tool is active and one or more raster/base/group layers are selected.
- Added eight resize handles and a separate rotation handle. Drag inside the box to move; Shift constrains corner scaling and snaps rotation to 15-degree increments.
- Kept interaction presentation-only: selected foreground and unselected background are prepared once, then pointer movement applies only a canvas matrix. The authoritative affine transform is committed on release and participates in Undo/Redo.
- Made full and dirty-region compositing, tight content bounds, nested groups, masks and painting coordinate conversion affine-aware.
- Preserved complete world transforms when layers are regrouped or dragged between transformed groups.
- Added position, width/height scale, rotation and Reset Transform controls to the Inspector.
- Fixed stale Inspector controls appearing behind the current page by recursively detaching and hiding nested row widgets/layouts before rebuilding, while deferring destruction safely when the rebuild was triggered by a control signal.
- Clarified CMake, About and documentation output: 0.2.3 always uses the CPU dirty-tile renderer. Supplying `wgpu-native` currently enables only an instance probe and does not improve performance until device/queue/pipeline dispatch is implemented.
- Added affine transform, dirty-region parity, hierarchy and project round-trip regression coverage.

## 0.2.2

Linux/Qt 6 compiler-fix release for document-state comparison.

- Replaced the implicitly deleted defaulted `LayerNode::operator==` with an explicit recursive state comparison. `QImage` is not equality-comparable in the way required by Qt 6's container traits, which prevented Undo/Redo document snapshots from compiling.
- Added an explicit layer-tree comparison helper so Undo/Redo change detection does not rely on Qt container equality traits.
- Raster and mask state are compared through Qt image revision keys plus image metadata, retaining constant-time comparisons across implicitly shared undo snapshots rather than scanning every pixel.
- Added a regression test covering layer-tree equality and raster copy-on-write revisions.
- No document-format or behavioural changes from 0.2.1.

## 0.2.1

Linux/GCC 16 compiler-fix release.

- Fixed two `std::clamp` calls in `PhotoDocument::groupLayers()` and `PhotoDocument::moveLayers()` where Qt 6's `QVector::size()` returned `qsizetype` while the destination indices were `int`.
- Added explicit integer conversion at the API boundary so GCC 16 can resolve the template consistently.
- No document-format or behavioural changes from 0.2.0.

## 0.2.0

Architectural correction pass for layer structure, live tools, transforms and history.

- Replaced QTreeWidget-owned drag mutation with a model-driven drop request. PhotoDocument is now the source of truth and the tree is rebuilt only after Qt has left its drag handler, fixing group children that appeared only after a later insertion.
- Added `PhotoDocument::moveLayers()` with nested-parent validation, selected-ancestor filtering, stable visual order and world-position preservation when moving between differently translated groups.
- Changed **New Group** so an existing Ctrl/Shift selection is wrapped into the new group. Selected descendants are not duplicated when an ancestor group is also selected.
- Added model tests for immediate group children, selected-to-group behaviour, selected-ancestor filtering and world-position preservation.
- Added backend-neutral dirty-region rendering through `RenderBackend`; tools no longer own an unrelated overlay compositor.
- Live Brush/Eraser preview now evaluates the actual layer tree for each dirty region, including layer opacity, blend mode, masks, groups and adjustment layers while the mouse button remains held.
- Cached the preview brush tip for the duration of a stroke and retained one authoritative full-resolution background commit on release.
- Added an optional isolated `wgpu-native` context and canonical WGSL kernels for brush stamping, tile compositing and transform presentation. CPU dirty-region evaluation remains the active reference/fallback until device, texture and pipeline dispatch are implemented.
- Reworked Move dragging so the selected foreground and unselected background are rendered once. Pointer movement changes only a presentation offset.
- Added tight visible-content transform bounds derived from alpha and effective masks, and reused content-aware bounds for guide snapping.
- Added a bounded command-based Undo/Redo stack covering structural layer changes, drag/drop, grouping, visibility, naming, opacity, blend modes, masks, adjustments, guides, brush/eraser strokes and transforms.
- Property slider drags coalesce into one undo command; automatically created Paint Layers are included in the same stroke command.
- Updated `.vfxphoto` to format version 4 while retaining loading support for versions 1–3.
- Updated both `run.sh` and `run.bat` to perform incremental builds before launching, preventing stale executables after overwriting a working tree.

## 0.1.12

Multi-selection, layer movement and responsive live-stroke preview.

- Added Ctrl/Shift multi-selection and bulk opacity, blend-mode, masks, remove and reordering.
- Added persistent document-space layer/group translation and the first multi-layer Move tool.
- Added presentation-scale live Brush/Eraser raster updates and one background full-resolution commit per stroke.
- Added group disclosure and refresh fixes that exposed the remaining Qt deferred drag/drop regression addressed in 0.2.0.
- Added `.vfxphoto` format version 3 for layer offsets.

## 0.1.11

Layer controls, group navigation and first raster-painting pass.

- Replaced the ambiguous text/symbol controls in the Layers panel with consistent runtime-painted icons for New Raster Layer, New Group, Adjustment, Mask, Move Up, Move Down and Remove.
- Made the layer-control icons palette-derived so the same drawing system can adapt to a future light theme.
- Added explicit, visible disclosure chevrons for groups and hardened drag source tracking so nested layers can be expanded, selected and reorganised reliably.
- Implemented a basic full-resolution Brush tool with primary colour, size, opacity and hardness.
- Implemented a basic Eraser tool for raster layers.
- Added a live document-scale brush outline on the canvas.
- Brush creates a new Paint Layer automatically when needed; Eraser asks for an existing raster layer.
- Preserved painted raster pixels through `.vfxphoto` save/load and extended the round-trip test accordingly.
- Versioned the workspace state for the refreshed Layers controls.

## 0.1.10

Linux compiler-fix release.

- Fixed `LayerTreeWidget` failing to compile because an internal tree-item helper collided with `QWidget::isAncestorOf`.
- Removed the unused colour-triangle parameter warning.
- Updated the Quit action to Qt 6's non-deprecated `QMenu::addAction` overload.

## 0.1.9

Hierarchical layers, blending and transparency foundation.

- Replaced the four separate adjustment buttons with one compact adjustment menu.
- Added transparent raster layers and nested groups, including groups inside groups.
- Replaced the flat layer list with an editable drag-and-drop tree.
- Added visibility toggles to every layer type, including the embedded base image and groups.
- Added selected-layer opacity slider/value controls and functional blend modes: Copy/Replace, Multiply, Screen, Overlay, Darken, Lighten, Colour Dodge, Colour Burn, Add, Difference and Exclusion.
- Added compact New Layer, New Group, Adjustment, Mask, Move Up, Move Down and Remove controls.
- Added white masks to raster, adjustment, base and group layers; masks are persisted and respected by preview/export, ready for later painting tools.
- Added checkerboard transparency when the base or other layers no longer cover the document.
- Added inline and Inspector renaming for all layers and groups.
- Added `.vfxphoto` format version 2 for recursive layer trees, raster payloads, masks, opacity and blend modes, while retaining version 1 project loading.
- Added tests for hidden-base transparency and recursive project round-tripping.

## 0.1.8

Compact colour-panel and selector-performance pass.

- Reorganised the Colour dock into three peer tabs: Colour, RGB and HSV.
- Kept the hue wheel, Hex entry and saved swatches together on the Colour tab while moving numeric models to their own pages.
- Reduced the primary/secondary swatches and removed the duplicated active-target/hex summary block.
- Kept Alpha synchronised between the RGB and HSV pages.
- Replaced per-frame swatch stylesheet rebuilding with lightweight painted icons.
- Stopped writing persistent settings and restyling all saved swatches during every colour-wheel mouse movement; the final colour is saved when the drag ends.
- Capped hue-triangle regeneration to a display-friendly cadence and reduced the wheel's preferred size.
- Reduced the default Colour dock allocation, made workspace reset return to the Colour tab, and versioned the workspace state for the compact layout.

## 0.1.7

Full colour-panel and sampling pass.

- Replaced the compact colour swatches with a hue wheel and saturation/value triangle selector.
- Added direct Hex, RGB, HSV and Alpha editing with all controls kept in sync.
- Made the primary and secondary swatches selectable editing targets rather than separate modal colour dialogs.
- Added sixteen persistent saved-swatch slots, with selection, replacement, clearing and first-empty-slot saving.
- Added a Colour-panel eyedropper button linked to the main toolbar tool.
- Implemented actual canvas colour sampling; click or drag over the rendered image to update the selected primary or secondary colour.
- Added sampled source-pixel coordinates and colour values to the status message.
- Expanded the default Colour dock allocation and versioned the workspace state for the new panel.

## 0.1.6

Canvas navigation and dock-tab placement polish.

- Moved the Layers/Channels dock tabs to the top of their shared panel group.
- Removed the duplicate active-dock title bar so the tabs themselves serve as the panel names.
- Added half-viewport canvas overscroll in free navigation mode, allowing every image edge and corner to be brought to the centre of the workspace.
- Kept zoom anchoring stable while entering and using the expanded canvas area.
- Hid the internal scroll bars; Move, middle mouse and Space-drag remain the intended navigation controls.
- Updated the workspace-state version so older saved layouts adopt the new top-tab arrangement.

## 0.1.5

Ruler-orientation and dock-tab polish.

- Corrected guide creation so the horizontal ruler creates horizontal guides and the vertical ruler creates vertical guides.
- Corrected ruler cursors to match the direction in which their guides move.
- Combined Layers and Channels into one tabbed dock group, with Layers selected by default.
- Updated the workspace-state version so existing 0.1.4 layouts adopt the new tabbed default cleanly.
- Reset Workspace Layout now explicitly returns focus to the Layers tab.

## 0.1.4

Workspace recovery, rulers and guides.

- Added persistent workspace layout storage and **View → Reset Workspace Layout**.
- Added source-pixel horizontal and vertical rulers.
- Added draggable, persistent guides with document-edge and midpoint snapping.
- Added ruler/guide visibility, snapping and clear controls.
- Stored guides in `.vfxphoto` projects.

## 0.1.3

Incremental-launch build fix.

- Changed `run.sh` to invoke the CMake/Ninja build before every launch.
- Builds remain incremental, so unchanged source files are not recompiled.
- Prevents an older executable from being launched after a newer project archive is copied over an existing working folder.

## 0.1.2

Workspace-foundation release.

- Replaced the original file/view button row with a contextual tool-options bar.
- Added a permanent vertical tool palette for Move, Brush, Eraser, Crop, Heal, Smudge, Fill, Gradient, Eyedropper and Text foundations.
- Renamed the Adjustment panel to Inspector and made the base image expose useful document information there.
- Reordered the right workspace into Colour, Inspector, Channels and Layers.
- Added persistent primary and secondary colours with choose, swap and reset controls.
- Added RGB, Red, Green, Blue and Alpha canvas preview modes.
- Added dedicated PNG tool and layer icons without introducing a Qt SVG dependency.

## 0.1.1

Build-fix release.

- Added the required `QColorSpace` includes in `PhotoDocument.cpp` and `ImageProcessor.cpp` for Qt 6 builds.
- Improved the Linux build script so missing build tools produce a useful Fedora installation command.

## 0.1.0

Initial project foundation.

- Added the Qt 6/C++20 desktop application.
- Added the zoomable and pannable image canvas.
- Added Exposure, Contrast, Saturation and Levels adjustment layers.
- Added asynchronous reduced-resolution preview rendering.
- Added full-resolution export.
- Added self-contained `.vfxphoto` project save/load.
- Added built-in 24/32-bit and RLE TGA reading plus 32-bit TGA writing.
- Added 8-bit and 16-bit integer processing paths.
- Added CMake presets, tests, Linux/Windows scripts and CI configuration.
