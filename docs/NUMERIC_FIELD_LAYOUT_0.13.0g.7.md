# 0.13.0g.7 — Numeric Field Layout and Inspector Stability

## Scope

This fix-up hardens the combined slider/value editor introduced in 0.13.0g.6 without changing its interaction model or any persisted document data.

## Tool Options sizing

All bounded scrub fields created for the Tool Options toolbar now use one 112 px width. This includes brush-family values, Crop Dim and Straighten, Fill Tolerance, Transform Snap Distance, vector stroke/shape controls and Corner Radius. Exact X/Y/W/H and effectively unbounded signed fields remain ordinary direct-entry controls. Popup stroke-appearance fields use the same compact width for a consistent visual rhythm.

The reusable control and its internal spin box have a fixed 30 px height. The spin-box vertical padding is removed so the progress fill is not inset or clipped at the bottom, while the Tool Options toolbar remains the established fixed 44 px row.

## Inspector stability

Layer opacity still updates the selected layers, live composite, status and transform overlay continuously. It no longer rebuilds the Inspector page for every intermediate drag value because opacity does not alter that page's structure. This removes scrollbar flicker and avoids needless destruction/recreation of Inspector widgets. The interaction still begins and finishes one grouped property Undo transaction.

## Compatibility

No project, preset, export, queue, recovery or colour-management schema changes are introduced. Existing 8-bit/16-bit, Alpha-safe hidden RGB, masks, selections, groups, Pass Through, GPU/CPU and multi-document residency behaviour is preserved.
