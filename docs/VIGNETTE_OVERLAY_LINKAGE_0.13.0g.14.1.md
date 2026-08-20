# 0.13.0g.14.1 — Vignette Overlay Linkage Fix

## Failure

The 0.13.0g.14 production target compiled the new on-canvas Vignette interaction calls but failed at the final link because the generated `ImageCanvas` MOC object did not provide the three newly declared signal bodies. The missing symbols were limited to interaction start, numeric change and interaction finish; the renderer and Vignette schema were not involved.

## Correction

`ImageCanvas` now exposes one explicit callback-registration method for the same start/change/finish contract. `MainWindow` installs callbacks that use the existing grouped property-Undo and preview paths, and clears them at teardown. The canvas regression records the callback values directly. This removes the new meta-object linkage dependency while retaining the established QObject signals used by all older canvas operations.

## Compatibility boundary

No adjustment parameter, renderer equation, project or preset field, tile-cache key, GPU/CPU path, Alpha contract, mask/group behaviour, colour-management operation, residency state, export snapshot or queue/recovery envelope changed. 0.13.0g.14 projects remain schema 16 and load identically.
