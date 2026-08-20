# 0.13.0g.3 — Transform Pixel Snapping and Gesture Responsiveness

## Scope

This fix-up completes the global snapping model for whole-layer Transform operations and removes the repeatable pause caused by preparing separated transform-preview surfaces synchronously on every gesture start. It does not begin 0.14.0 and does not introduce Smart Layers or live-filter persistence.

## Whole-layer pixel lattice

With the global Snap control enabled, Transform Move quantises the resulting transformed top-left boundary for raster, vector, text and group roots to integer document coordinates. This means older layers with fractional transforms are pulled back onto the pixel lattice rather than preserving their old offset. Existing magnetic targets remain active, but only whole-pixel corrections are accepted, so a half-pixel guide cannot pull a layer edge off the pixel lattice. Holding Ctrl uses the existing temporary snapping bypass and leaves the pointer delta subpixel-precise.

For axis-aligned resizing, only the actively scaled document coordinate is rounded. Magnetic scale targets must themselves be integer boundaries. Edge handles leave the orthogonal coordinate untouched, and rotated/skewed/distorted/perspective geometry is not forcibly rounded. This keeps filled vectors and raster extents clean without damaging non-axis-aligned geometry.

## Transform preview preparation

The previous gesture-start path rendered two complete preview surfaces on the UI thread: a layer tree with selected roots hidden, and a tree retaining only the selected roots. Large images therefore paused before the first visible Move, Scale or Rotate update, and translation auto-commit repeated the cost on every new drag.

The Move-tool idle path now snapshots the required immutable inputs and prepares both surfaces asynchronously through the existing native RenderBackend with the exact CPU fallback retained. The result is accepted only when session identity, render serial, channel view and selected root IDs still match. Starting a transform consumes a ready result without repeating the renders on the UI thread.

After a pure translation commits, the selected-hidden background is still valid. The cache therefore retains that background and applies the committed document translation to the prepared selected-only foreground, while rebuilding bounds and snap targets from the updated layer tree. Subsequent moves can begin immediately. Non-translation commits invalidate the transient cache and schedule normal idle preparation for the resulting geometry.

## Compatibility and safety

- No project, adjustment, vector, preset, residency, export, queue or recovery schema changes.
- No change to raster bake interpolation or semantic vector/text live-preview behaviour.
- Native GPU rendering and exact CPU fallback remain the only surface-rendering paths.
- Worker requests use copied/implicitly shared snapshots and cancellation tokens; they do not capture MainWindow or live document references.
- Session switches, new documents and shutdown cancel or wait for outstanding preparation work.
- Existing Alpha-safe hidden RGB, editable channels, masks, selections, groups, Pass Through, 8/16-bit processing, ICC, OpenColorIO and ACES contracts remain unchanged.
