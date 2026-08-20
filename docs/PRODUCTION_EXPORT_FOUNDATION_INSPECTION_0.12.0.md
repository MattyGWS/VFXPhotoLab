# 0.12.0 source inspection — presets and production export

This inspection records the accepted 0.11.0i.3 implementation boundaries used to plan 0.12.0. It is descriptive: it does not change the legacy project or rendering contracts.

## Existing preset paths

- `AdjustmentPresetStore` owns built-in and user adjustment presets. User files were version-1 per-adjustment JSON under `adjustment-presets/<type>`. The payload is a complete `AdjustmentData`, so LUT presets already retain embedded generic LUT, Tony McMapface and AgX state through the normal adjustment serializer.
- `MainWindow::addAdjustmentPresetControls()` exposes the existing adjustment preset selector, save/overwrite and delete flow in the Inspector.
- `VectorAppearancePresetStore` owns version-1 JSON files under `presets/vector-appearance`. `MainWindow` already provides a vector appearance manager with save/replace, apply, rename and delete controls.
- Adjustment defaults and parameter persistence remain part of `AdjustmentData` and layer serialization. Vector appearance remains part of `VectorAppearance` and vector-layer serialization. Neither should be reinterpreted by a generic preset layer.
- There was no shared identifier, metadata, category, favourite/recent or import/export contract across these stores.

## Existing export path

- `MainWindow::exportImage()` is the approachable single-image workflow and remains the quick-export entry point.
- `ImageExportDialog` gathers one output path and the current format/bit-depth/colour-profile/rendering-intent/dithering/Alpha options.
- `ImageExport` performs the full-resolution colour-managed render and encoding. It already accepts cooperative cancellation and publishes through `QSaveFile`, giving the single-output path an atomic final write.
- Existing overwrite confirmation and file-format capability checks belong to the current dialog/export path and must remain available to quick export.
- There was no reusable export-profile model, naming-template parser, multi-output job description or persistent export queue.

## Colour and document state

- `DocumentColourState::output` already carries document output defaults and travels through public project persistence and private Hot/Warm/Cold snapshots.
- Monitor ICC, OCIO Display/View, soft proof and gamut warning remain presentation-only and are not valid production-export inputs unless an explicit future output profile requests an output transform.
- Export-profile presets should therefore reference explicit output settings and must not copy transient display state.

## Background work, cancellation and failure boundaries

- The current export worker and renderer already have cooperative cancellation points. 0.12.0 should compose those operations rather than add a second pixel pipeline.
- Atomic output publication already exists at the single-file encoder boundary. Multi-output export must retain one atomic write per output and isolate failures so completed outputs survive a later failure.
- Queue cancellation must stop future outputs and cooperatively cancel the active output without blocking the UI thread.

## Multi-document and residency boundaries

- Each open document is isolated by `DocumentSession`; Hot/Warm/Cold transitions serialize document state independently.
- Preset-library metadata is application-global. Document-specific production-export configuration should be copied into the owning document/session only where it represents deliberate document state.
- Queue jobs must capture immutable document/job descriptions or an explicit render snapshot. They must not retain unsafe pointers to a document that can be closed, cooled or restored.

## 0.12.0 implementation sequence

1. **0.12.0a — Unified Preset Architecture** — complete
2. **0.12.0b — Preset Management UX** — complete
3. **0.12.0c — Export Profiles and Naming Templates** — complete
4. **0.12.0d — Production Multi-Output Export** — complete
5. **0.12.0e — Export Queue Foundation** — complete
6. **0.12.0f — Recovery and Session Persistence** — complete
7. **0.12.0g — Integration and Hardening** — complete

**0.12.0 Presets and Production Export Foundation is complete for testing.** Folder-wide batch processing and automation remain outside this milestone and are reserved for 0.19.0.
