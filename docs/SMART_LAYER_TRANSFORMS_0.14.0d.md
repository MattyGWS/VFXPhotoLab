# 0.14.0d — Non-Destructive Smart Transforms

## Scope

0.14.0d makes Smart Layer transforms explicitly source-backed. It does not implement the final Smart source-tile/cache system (0.14.0e), Live Filters, Layer Effects or linked external sources.

## Authoritative contract

A Smart instance owns presentation state, not source pixels:

```text
SmartLayerInstance
  Smart Source ID / observed revision
  Transform
  Smart transform sampling
  Mask
  Opacity / blend mode
```

The document-owned Smart Source remains authoritative. Applying a Smart transform modifies only the instance transform and its sampling state. No transformed raster is written into the embedded source or instance raster payload. Consequently repeated scaling always evaluates the current source presentation.

## Sampling

`SmartTransformState` schema 1 persists Nearest, Bilinear, Bicubic or Lanczos 3. Bilinear is the migration default because 0.14.0a-c used Qt's smooth presentation path. Bicubic/Lanczos use `TransformSampling` in straight RGBA64 so RGB beneath transparent Alpha is not destroyed by premultiplication. Smart instance masks use the same state through the 16-bit grayscale reference sampler.

The exact CPU path computes the inverse-mapped source footprint for each requested output region, expands it for the interpolation kernel, and converts only that footprint to the reference format. This avoids whole-source conversion per tile while remaining a deterministic parity oracle for 0.14.0e.

## Interaction proxy

The Transform tool already prepares an immutable foreground surface when the gesture begins. For Smart Layers the pointer loop updates only the foreground matrix. Apply composes the transform into the Smart instance and stores the selected sampling method. This prevents source re-rendering on every move/rotate/scale pointer event.

The proxy is deliberately an interaction cache, not authoritative transformed content. The settled compositor result is regenerated from the current Smart Source presentation.

## Source revisions

Edit Contents can update a Smart Source after an instance has been transformed. Source revision propagation refreshes the Smart presentation, while the existing instance transform and sampling state remain unchanged. This is the required basis for linked-source updates later in 0.14.0l.

## Persistence

- Public `.vfxphoto`: version 20.
- Embedded Smart document: schema 3.
- Private Hot/Warm/Cold snapshot: version 21.
- Pre-20 Smart instances with no transform metadata load as Bilinear.
- Pre-schema-3 embedded payloads cannot contain nested Smart transform metadata.
- Pre-21 private snapshots cannot claim the new per-layer payload.

## Deferred to 0.14.0e

The Smart Source still owns the bounded whole-source derived presentation introduced in 0.14.0b. 0.14.0e will replace that interim cache with transform-aware source-tile requests, dirty-region propagation, revision-keyed intermediate caches, GPU residency and bounded selective invalidation.
