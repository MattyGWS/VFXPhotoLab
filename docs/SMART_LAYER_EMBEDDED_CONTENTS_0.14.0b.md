# 0.14.0b — Smart Layer Embedded Contents and Conversion

## Ownership contract

A `SmartSourceDescriptor` now owns an authoritative `embeddedDocument` payload and a revision-keyed derived presentation. A parent `LayerNode` of type `Smart` stores only `SmartLayerReference` plus normal instance state. Runtime presentation image fields on the layer are non-authoritative, omitted from layer JSON/session payloads, and rebound from the registry.

The embedded document reuses the normal `LayerNode` JSON contract so masks, raster/vector/text/adjustment/group state, transforms, nested Smart references and Pass Through metadata are not translated into a second model. Embedded metadata also records canvas extent, resolution, colour model and the full managed colour state.

## Convert to Smart Layer

`PhotoDocument::convertLayersToEmbeddedSmart()` performs an atomic prepared-state conversion. Selected root layers are detached intact, legacy Base Image pixels are materialised when necessary, nested Smart dependencies are extracted, the embedded payload and exact presentation are built, a new Smart Source is registered, and one Smart Layer instance is inserted at the original stack position.

The public UI exposes **Layer → Convert to Smart Layer** and the same command in the Layers context menu. MainWindow captures the complete pre/post structural state so Undo restores the original layer tree and source registry, while Redo restores the Smart source and rebinds its presentation.

## Exact-appearance boundary

Copy/opacity composition is associative over transparency, so ordinary contiguous sibling layers can be embedded without requiring unrelated backdrop layers. A top-level Adjustment Layer, non-Copy blend or Pass Through group can depend on content below its selection. 0.14.0b therefore accepts such a selection only when it contains every lower sibling in a root/isolated context. Other cases are rejected with an explicit message rather than approximated.

Selections from unrelated parents and non-contiguous sibling ranges are also rejected in this revision because collapsing interleaved hierarchy into one stack position cannot preserve exact appearance without duplicating or restructuring unselected ancestors/siblings.

## Rendering and persistence

The authoritative embedded source remains editable semantic content. 0.14.0b derives one bounded full-quality presentation for the current source revision, preserving 8/16-bit processing and meaningful hidden RGB across the embedded raster storage bounds before compacting only all-zero RGBA exterior pixels. This cache is shared by instances and feeds the existing compositor/tile engine; it is not the Smart source itself. The interim cache is capped at 64 Mpix and 256 MiB uncompressed so pathological conversions fail explicitly instead of allocating without bound. 0.14.0e will replace this whole-source presentation strategy with transform-aware source tile requests and intermediate caches.

`.vfxphoto` is format 18. Hot/Warm/Cold snapshots are format 19. Format-17/18 state from 0.14.0a remains readable, while older envelopes cannot claim to contain 0.14.0b embedded payloads.

## Deferred intentionally

- **0.14.0c:** Edit Contents as a normal multi-document VFX Photo Lab document, save/update propagation and source-document lifecycle.
- **0.14.0d:** authoritative source-backed non-destructive transforms and transform proxies.
- **0.14.0e:** tiled source requests, transform-aware cache architecture and bounded intermediate residency.
- Live Filters and Layer Effects remain distinct later stages of 0.14.0.
