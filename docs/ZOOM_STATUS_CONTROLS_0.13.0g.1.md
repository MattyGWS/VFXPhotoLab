# 0.13.0g.1 — Bottom Status Zoom Controls

## Scope

This post-milestone fix-up expands the existing bottom-right zoom readout into a compact navigation cluster. It does not add a toolbar Zoom Tool and does not alter mouse-wheel navigation.

## Controls

The status bar now presents:

- **Zoom Out** to the previous discrete stop;
- the live zoom percentage;
- **Zoom In** to the next discrete stop;
- **1:1** for Actual Pixels at 100%;
- **Fit** for a fresh fit-to-current-viewport calculation.

Buttons do not accept keyboard focus, so clicking them does not disrupt canvas tool shortcuts. Tooltips and accessible names describe every action. Controls are disabled when no document is open, and the directional buttons disable at the established 2% and 3200% limits.

## Stop policy

From 25% upward, directional buttons move to strict 25% boundaries. A button always moves away from an exact boundary rather than selecting the current value again. Examples:

- 183% → 175% or 200%;
- 175% → 150% or 200%;
- 26% → 25% or 50%.

Below 25%, the useful 6.25% and 12.5% stops are retained, followed by the existing 2% minimum. The percentage readout keeps those fractional values visible instead of rounding them to misleading whole numbers.

## Navigation contracts

Stepped zoom and Actual Pixels use the established centred `ImageCanvas::setZoom()` path, preserving the visible canvas centre. Fit reuses the durable fit mode and recalculates from the current viewport dimensions. Smooth wheel zoom remains unchanged and continues to anchor beneath the pointer.

## Compatibility

No persisted data changes. Project format 15, adjustment schema 15, Hot/Warm/Cold schema 16, colour schema 4, vector schema 7, presets, production export, queue recovery, ICC/OCIO/ACES processing, Alpha-safe hidden RGB and GPU/CPU rendering contracts remain unchanged.
