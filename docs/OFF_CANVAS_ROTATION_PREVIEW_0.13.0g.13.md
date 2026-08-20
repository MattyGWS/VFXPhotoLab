# 0.13.0g.13 — Off-Canvas Rotation Preview Retention

## Failure

0.13.0g.12 retained pixels across completed pure translations, but an applied raster deformation still exposed an older clipping boundary. Raster rotation is baked intentionally so painting tools continue to edit an axis-aligned compact image. The baked image and mask retain all transformed pixels, and `rasterReferenceOrigin` records where that compact storage belongs in document space—even when the entire result lies outside the canvas.

The next Transform session did not use that storage extent. It separated the selected foreground by rendering into a document-sized preview surface. Document tiles cannot represent coordinates outside the canvas, so the off-canvas portion was absent from the preview even though it remained present in the authoritative layer. Moving the layer back appeared empty until Apply or another settled document render rebuilt the image from real layer storage.

## Correction

Transform foreground preparation now returns a pair:

- a presentation image for the selected content; and
- the exact document-space bounds represented by that image.

Content bounds are mapped to preview coordinates with a small sampling pad. Regions contained by the source canvas continue through the native region renderer and are placed into the established full-document foreground surface, preserving equal-extent GPU transform compositing. A region extending outside document tiles uses `ImageProcessor::renderUnclippedRegion()`, an exact CPU reference that evaluates the same layer hierarchy over arbitrary preview-space coordinates and includes the complete spatial-adjustment halo before cropping to a compact off-canvas surface.

`ImageCanvas` now stores the foreground presentation bounds separately from the transform selection bounds. Its live QPainter path and temporary preview commit draw compact surfaces into those bounds before applying the current/base document transform. The native GPU transform-composite path still operates for its established full-document, equal-extent contract; compact off-canvas surfaces deliberately use the exact presentation path rather than uploading a misleading clipped image.

The 0.13.0g.12 move cache also carries these bounds, so retained source pixels, accumulated translation and compact placement remain one coherent preview state.

## Preserved contracts

- Applied rotation remains a real raster deformation with explicit Apply/Cancel.
- Pure whole-layer translation still commits on release as one Undo operation.
- Authoritative raster and mask storage are not flattened to document bounds.
- Alpha-safe hidden RGB, editable channels, masks, isolated/Pass Through groups, selections, 8/16-bit processing and colour-management compatibility are unchanged.
- Project and preset formats are unchanged.
- The ordinary tiled/GPU preview route remains preferred for in-canvas regions, with honest exact fallback.

## Regression coverage

- `CoreTests::unclippedTransformRegionRendersOffCanvasStorage()` verifies arbitrary-region compositing can recover compact raster storage wholly beyond the source rectangle.
- `CanvasTests::transformPreviewBoundsRestoreBakedOffCanvasRotation()` verifies a compact rotated payload at off-canvas document bounds becomes visible when transformed back and that preview commit is pixel-identical to the live presentation.
