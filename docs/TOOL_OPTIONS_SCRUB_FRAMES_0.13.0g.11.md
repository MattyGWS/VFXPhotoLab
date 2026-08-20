# 0.13.0g.11 — Complete Tool Options Scrub Frames

## What the screenshots established

The 28 px Tool Options fields were positioned correctly and remained wholly inside the fixed 44 px toolbar. Their top and side borders rendered, but the final bottom scanline was the input-background colour. The same reusable control in Colour, Inspector and Layers rendered a complete 30 px frame. This ruled out another toolbar-height or vertical-alignment adjustment.

## Root cause

Qt style-sheet `min-height` and `max-height` values describe the styled content box. The compact control had both of these contracts at once:

- a physical `QWidget`/`QDoubleSpinBox` height of 28 px; and
- a toolbar stylesheet content height of 28 px.

The spin box also has a 1 px top border and 1 px bottom border. The requested painted box was therefore 30 px high inside a physical 28 px widget. Qt retained the upper edge and clipped the final two pixels, which removed the lower border and rounded corners. The 30 px side-panel field did not receive the toolbar-specific 28 px content rule and therefore remained complete.

## Correction

The physical Tool Options wrapper and spin box remain 28 px. Only the toolbar-specific stylesheet content height is now 26 px:

- 26 px content;
- 1 px top border; and
- 1 px bottom border.

The total painted box is exactly 28 px. No toolbar enlargement, arbitrary translation, margin compensation or platform-specific branch is used.

## Regression coverage

The widget test now applies the real application palette and stylesheet to a `ToolOptionsToolbar`, renders the compact scrub control to an image, and checks that the centre pixel of the final scanline equals the active theme's border colour. The existing geometry assertions remain, so both layout containment and actual frame painting are covered.

## Compatibility

This revision changes one toolbar-only style metric and its focused test. It does not alter scrub values, Undo grouping, tool state, project or preset serialization, crop/transform behaviour, GPU/CPU rendering, Alpha-safe hidden RGB, masks, groups, colour management, multi-document residency, exports, queue snapshots, recovery or the 0.13.0g.10 shutdown fix.
