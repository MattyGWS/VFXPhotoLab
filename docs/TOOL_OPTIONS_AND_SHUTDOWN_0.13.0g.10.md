# 0.13.0g.10 — Tool Options Field Geometry and Shutdown Safety

## Tool Options clipping traced

The combined scrub/value control is reused in two different layout environments:

- Colour, Inspector, Layers and adjustment panels give it a comfortable 30 px row and render it correctly.
- Tool Options is a fixed 44 px toolbar whose ordinary buttons, combo boxes and spin boxes are styled to a 28 px action row.

The scrub wrapper and its native `QDoubleSpinBox` were always fixed to 30 px, while Tool Options also applied toolbar-specific constraints. KDE Breeze lays out embedded `QWidgetAction` controls using the toolbar action row and can therefore present a 30 px child through a 28 px effective control rectangle. The mismatch is small enough to affect only the lower border/corner, and device-pixel-ratio rounding makes it more visible at some scaling values. Increasing the toolbar again would only hide the conflict and move the ruler/canvas.

`SliderSpinBox` now exposes one explicit height contract. Its default remains 30 px for dock and Inspector rows. Every Tool Options construction path requests 28 px, and the wrapper, native spin box and toolbar style sheet all use that same integral logical height. The fixed toolbar remains 44 px and all other control metrics are unchanged.

## Exit crash traced

The attached Fedora core trace terminates in this sequence:

1. `DocumentSession` begins destruction.
2. Its owned `QUndoStack` destructor clears history and emits `cleanChanged`.
3. MainWindow's session callback is still connected and calls `updateWindowTitle()`.
4. The title refresh updates the active document-strip entry.
5. `DocumentStripWidget::setActiveDocument()` changes the Qt selection model while the session/member graph is already being torn down.

The crash is therefore not a Vulkan or Mesa failure despite GPU worker threads being present in the report. The main-thread stack identifies a UI re-entry/lifetime violation during ordinary C++ member destruction.

MainWindow now establishes a teardown barrier at the start of its destructor:

- mark shutdown active;
- clear the QUndoGroup active stack;
- disconnect every session `QUndoStack` from MainWindow;
- remove every stack from QUndoGroup;
- clear registered session-signal IDs; and
- reject title or document-strip refreshes after shutdown begins.

This leaves each `DocumentSession` free to destroy its Undo history later in normal member order without invoking UI code. Existing export-queue shutdown, transform-prewarm cancellation and Tool Options action disposal remain in their established order.

## Compatibility

This revision changes only widget geometry and shutdown signal lifetime. It does not alter projects, presets, Undo command contents, crop/transform state, layer compositing, Alpha-safe hidden RGB, 8/16-bit processing, ICC/OCIO/ACES, Hot/Warm/Cold residency, exports, queue snapshots or recovery data.

## Regression coverage

The canvas/widget test suite now verifies that:

- the reusable scrub field remains 30 px by default;
- Tool Options can assign a matching 28 px wrapper and spin-box height;
- the compact field remains wholly inside a 44 px toolbar row; and
- a session-style Undo stack is disconnected from its UI receiver and removed from QUndoGroup before destruction, producing no late UI refresh.
