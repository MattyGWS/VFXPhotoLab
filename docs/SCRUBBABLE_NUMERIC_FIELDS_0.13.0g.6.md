# 0.13.0g.6 — Fixed Tool Options and Scrubbable Numeric Fields

## Purpose

This fix-up removes the one- or two-pixel vertical workspace movement that could occur when changing tools and establishes one reusable compact numeric editor for bounded visual parameters.

No project, preset, adjustment, colour-management, export, queue or recovery schema changes in this revision.

## Invariant Tool Options row

The Tool Options toolbar now uses a fixed-height toolbar class whose normal and minimum size hints both report the same 44 px row height. Its child labels, buttons, combo boxes and numeric fields also use one consistent control height.

Changing tools may replace the toolbar contents, but it cannot change the toolbar row height or move the rulers and canvas.

## Combined scrub-and-type control

`SliderSpinBox` now occupies one numeric-field footprint rather than a separate slider and spin box.

- A theme-aware fill behind the value shows its position in the valid range.
- Click and release enters ordinary text editing.
- Horizontal movement beyond Qt's platform drag threshold starts scrubbing.
- Double-click selects the complete value for replacement.
- Shift-drag uses one tenth of the normal sensitivity.
- Ctrl-drag uses ten times the normal sensitivity.
- Enter commits typed text.
- Escape restores the value that was present when editing received focus.
- Mouse-grab loss, window deactivation and widget removal safely finish an active scrub.

Ranges that cross zero render their fill outward from a visible zero marker so positive and negative adjustments remain legible. Ordinary bounded ranges use a practical range-aware scrub speed. Very large technical ranges retain fine control near small values and scale sensitivity with the current value's magnitude.

## Undo and publication contract

The existing `interactionStarted` and `interactionFinished` signals remain the transaction boundary:

- one complete scrub produces one property Undo transaction;
- one typed edit produces one property Undo transaction;
- a click that makes no change produces no history entry;
- the release/edit-finished path publishes the final value;
- Inspector destruction cannot strand an unfinished property transaction.

## Migration audit

The combined control now covers the reusable adjustment controls and suitable bounded visual values in:

- adjustment Inspectors and Gradient Map stop position;
- RGB/HSV/Alpha colour channels;
- Layers opacity;
- Brush, Eraser and retouching Tool Options;
- Transform snap distance;
- vector shape/Pen stroke and parameter controls;
- Fill tolerance and Corner radius;
- Crop overlay dim amount and straighten angle.

Exact-entry construction data remains deliberately unchanged, including document/canvas dimensions, crop and transform X/Y/W/H fields, and effectively unbounded signed dash offsets.
