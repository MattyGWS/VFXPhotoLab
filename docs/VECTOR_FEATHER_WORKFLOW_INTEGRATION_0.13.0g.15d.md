# 0.13.0g.15d — Vector Feather Compositing and Workflow Integration

## Scope

This revision integrates the exact non-destructive vector-layer Feather renderer from 15b/15c through workflows that reason about vector extent or can restructure editable vector content. It does not change the Feather data schema or invent a second rendering path.

## Compositor ordering

The existing compositor already has the required order and remains authoritative:

1. Resolve editable vector geometry and render the combined fill/stroke appearance through `VectorRasterizer`.
2. Feather the combined silhouette using the exact CPU renderer or parity-approved native tiled GPU coverage path.
3. Apply the vector layer's ordinary raster mask as a separate coverage input.
4. Apply layer opacity and blend mode during compositing.
5. Recurse through Isolated or Pass Through groups using the same layer result.

This keeps masks editable and prevents Feather from becoming a hidden mask or live Gaussian Blur. Adjustment layers and their masks continue to operate on the already-composited image exactly as before.

## Bounds, selection and transforms

`VectorRasterizer::contentBounds()` is now used by Reveal All and Fit Canvas to Selected Layers. It resolves transformed fill/stroke geometry and expands it by the Feather radius in document pixels, so geometry that is outside the canvas can still contribute a soft halo back into it.

Semantic vector bounds remain in use for path nodes, snap points and transform pivots. Feather is appearance around geometry; it must not move anchors or change the user's geometric pivot. Ordinary layer Transform, rotation and scale therefore leave the numeric Feather value unchanged while the renderer applies it around the transformed silhouette in document pixels.

Ctrl-click/selection coverage already uses `VectorRasterizer::contentBounds()` and `renderLayerRegion()`, so it naturally sees feathered Alpha rather than the raw path only. Multi-selected transforms remain layer-specific and preserve each layer's independent Feather value.

## Image Size

Image Size changes the document-pixel coordinate system itself. The vector payload already scales stroke widths, dash lengths and other pixel-radius appearance values by the conservative minimum-axis factor. 15d applies the same factor to Feather. Values that would exceed `VectorLayerData::MaximumFeatherRadius` reject the prepared resize rather than being silently clamped.

## Expand Stroke

At zero Feather, the established Expand Stroke behaviour is unchanged: a simple stroke-only object can remain one vector layer, while fill/stroke or multi-object cases may use an isolated group to preserve draw order and layer-level state.

For non-zero Feather, splitting fill and expanded stroke into child layers is not exact because each child would acquire its own Feather boundary. 15d therefore keeps all generated retained-fill and expanded-stroke objects inside one editable vector layer in render order. The original layer transform, mask, opacity, blend mode and Feather are applied once to that combined silhouette.

If the combined editable object count would exceed the existing safety limit, the operation is rejected before mutation.

## Merge Layers

The provisional 15a rule allowed editable vector layers to merge when their Feather values matched. The exact renderer shows why that is still lossy: two source layers are feathered independently before they are composited, whereas one merged vector layer would feather the union of their geometry once. Overlap and nearby boundaries can therefore change.

15d rejects editable vector Merge whenever any participating vector layer has non-zero Feather. The error explains that the user can set Feather to `0 px` for an exact editable merge or flatten through an explicit raster/export workflow. Zero-Feather vector merge remains unchanged and exact.

## Persistence, residency and exports

No new persistence envelope is required. Vector schema 8 stores Feather; project format 16 and Hot/Warm/Cold snapshot format 17 already preserve it. Copy, duplication and inserted vector copies continue to preserve the independent value.

Quick Export and Production Export both render from immutable document state through `ImageProcessor::renderPreservingHiddenRgb()`, so the exact Feather result, ordinary masks, hidden RGB and colour-management/export contracts are shared with the canvas compositor. No export-only Feather implementation is added.

The current application has no standalone destructive Vector → Raster Layer command. Existing explicit flatten/render paths use `VectorRasterizer`/`ImageProcessor`, so they receive Feather automatically.

## SVG compatibility decision

Standard SVG has blur/filter primitives, but none exactly represents VFX Photo Lab's deterministic fractional three-box filter applied to one combined fill/stroke silhouette while preserving the application's editable appearance semantics. Emitting `feGaussianBlur` would be an approximation; rasterising would destroy editability; omitting the VFX state would lose Feather.

15d therefore:

- keeps exact vector/Feather state in the existing `data-vfx-vector-data` round-trip payload;
- emits `data-vfx-feather-radius` for explicit diagnostics/interchange awareness;
- exports ordinary editable SVG geometry without an approximate filter;
- emits an explicit warning that external SVG viewers show the unfeathered vector appearance;
- restores exact Feather when the SVG is re-imported into VFX Photo Lab.

No silent rasterisation, approximate Gaussian substitution or persisted-value discard occurs.

## Compatibility

- Vector schema: **8** (unchanged)
- `.vfxphoto` project format: **16** (unchanged)
- Hot/Warm/Cold snapshot format: **17** (unchanged)
- Export profile/plan/queue/recovery schemas: unchanged
- 0 px rendering: unchanged exact fast path
- Smart Layers and Live Filters: not started; still **0.14.0**

Final vector Feather hardening remains **0.13.0g.15e**.
