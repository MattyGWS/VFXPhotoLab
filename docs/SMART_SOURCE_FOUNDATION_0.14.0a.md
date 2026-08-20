# 0.14.0a — Non-Destructive Layer Architecture and Smart Source Foundation

This revision begins the 0.14.0 Smart Layers, Live Filters and Core Layer Effects milestone by establishing durable ownership and invalidation contracts before any large feature UI is exposed.

## Architectural decisions

### Smart Layer instance versus Smart Source

A Smart Layer is now an explicit `LayerType::Smart`. The layer instance owns ordinary presentation state such as its layer ID, transform, mask, opacity and blend mode, plus a lightweight `SmartLayerReference` containing a persistent source ID and the source revision observed by the instance.

The authoritative source identity lives separately in the document-owned `SmartSourceRegistry`. Each `SmartSourceDescriptor` owns:

- a persistent UUID source identity;
- a monotonic source revision;
- Embedded or Linked storage intent;
- a display name;
- the linked path for Linked sources; and
- explicit source-to-source dependencies.

This intentionally prevents normal raster layers from becoming source-backed implicitly and gives later Smart Layer instances independent presentation state while sharing one source identity.

### Dependency and revision contract

The Smart Source dependency graph must remain fully resolvable and acyclic. Insert/replace operations validate the complete graph transactionally. A source cannot be removed while another source depends on it, and a source still referenced by a Smart Layer cannot be removed from the document.

Source edits use the registry revision-invalidation path rather than arbitrary descriptor replacement. Bumping one source revision also bumps every transitively dependent source revision, and only Smart Layer instances that reference one of those affected source IDs have their instance revision invalidated. This is the cache/dependency contract that later Edit Contents and linked-source updates will use.

### Semantic pipeline separation

0.14.0 keeps these concepts distinct even where later renderer infrastructure is shared:

`Layer Content / Smart Source -> Transform -> Live Filters -> Layer Effects -> Layer Mask -> Opacity -> Blend Mode`

0.14.0a introduces only the Smart Source/instance ownership portion. It does **not** create a generic effects array that mixes Live Filters and Layer Effects. Their separate persistent stacks are intentionally deferred to 0.14.0f and 0.14.0h after Smart content/render ownership is established.

### Rendering and cache identity

The existing tiled compositor now recognizes `LayerType::Smart` and includes Smart source ID, observed source revision and layer revision in the Smart Layer composite hash. This prevents future source revisions from accidentally reusing an instance presentation cached for an older source revision.

0.14.0a intentionally does not expose Smart Layer creation/conversion in the UI and does not invent a flattened or low-quality Smart renderer. The core renderer treats an API-level Smart Layer as having no renderable source payload until embedded source contents are implemented in 0.14.0b. This keeps the foundation honest rather than baking temporary pixels into the new model.

The existing `TileDomain::Source` remains available for the source-tile cache work planned for 0.14.0e. No whole-document Smart reprocessing path is added here.

## Persistence and compatibility

Public `.vfxphoto` format is now **17**. Version 17 writes a top-level `smartSources` registry and Smart Layer nodes write their source reference. Versions 1–16 remain readable and cannot claim Smart Layer state that their schema did not contain.

Hot/Warm/Cold private session snapshots are now **18**. Snapshot 18 writes the same source registry and Smart Layer source reference alongside the existing exact layer, colour-management, selection and editor state. Older supported snapshot versions remain readable under their established migration rules.

Structural document Undo/Redo snapshots now carry the Smart Source registry together with the layer tree. This avoids restoring a Smart Layer instance without the source identity graph that makes the instance valid.

Linked paths are stored exactly as supplied in this foundation. Canonical path policy, missing-link UX, file watching, Replace/Relink and Embed Linked Source remain 0.14.0l work; a missing linked path is never silently substituted here.

## Deliberately deferred to focused revisions

- **0.14.0b:** embedded Smart Source document payload, exact single/multi-layer/group conversion, internal canvas/origin model and Undo-visible conversion workflow.
- **0.14.0c:** normal multi-document Edit Contents, source-document identity/revisions, open-parent propagation and nested-source cycle checks driven by real embedded contents.
- **0.14.0d:** authoritative source-backed non-destructive transforms and transform proxies.
- **0.14.0e:** transformed source tile requests, dirty regions, source/intermediate caches, residency and bounded GPU memory.
- **0.14.0f–g:** distinct Live Filter persistence/render/UI/masks/GPU integration.
- **0.14.0h–k:** distinct Layer Effect (`fx`) persistence/coverage pipeline/UI and core effects.
- **0.14.0l:** externally linked source workflows.
- **0.14.0m:** full workflow/export/recovery/performance hardening.

No requested 0.14.0 feature is removed by this split; the revision establishes the contracts the later stages depend on.
