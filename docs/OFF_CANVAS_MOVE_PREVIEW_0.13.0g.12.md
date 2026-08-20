# 0.13.0g.12 — Off-Canvas Move Preview Retention

## Failure

Whole-layer translation is committed immediately when a Move gesture ends. The authoritative layer transform retained all raster pixels, including pixels beyond document bounds. The transform preview cache did not: `rebuildMovedTransformPreviewCache()` translated the selected foreground into another document-sized image. Any part outside that image was discarded. A following drag therefore used an incomplete or fully transparent cached foreground until a settled document render replaced it.

## Correction

The cache now keeps two independent values:

- The unchanged pre-move foreground surface.
- Its accumulated document-space placement transform.

Each completed Move gesture composes its translation onto the placement transform. A following gesture begins from that accumulated placement and applies only its new session transform. Pixels may leave and re-enter the canvas repeatedly without being removed from the live source.

The same combined transform is used by:

- The direct QPainter transform preview used for translation.
- Native GPU preview composition when the combined operation requires deformation.
- Preview commit before the authoritative settled render arrives.

## Interaction contract

Pure movement continues to commit on pointer release. It creates one `Move Layer` or `Move Layers` history command and does not leave a pending transform requiring Apply or double-click. Scale, rotate, skew, distort and perspective retain their established pending Apply/Cancel workflow.

## Preserved behaviour

No layer pixels are rewritten by this change. Raster storage origins, masks, isolated and Pass Through groups, selections, editable channels, Alpha-safe hidden RGB, 8/16-bit precision, native GPU rendering, honest CPU fallback, project persistence, colour management and export paths remain unchanged.
