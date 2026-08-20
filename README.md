# VFX Photo Lab

The current development release is **0.14.0m.2 — Layer Effect Owner-Selection UX Fix**.

This is the final implementation stage of **0.14.0 — Smart Layers, Live Filters and Core Layer Effects**. It does not add a new persisted editing subsystem; it hardens the interactions between the Smart Source architecture from 0.14.0a–l, Live Filters, the complete core `fx` stack, residency/history/recovery and export.

## 0.14.0m.2 Layer Effect Owner-Selection UX Fix

- Selecting an individual Layer Effect now keeps that effect as the focused Layers-panel sub-item while resolving canvas/tool operations to its owning layer.
- Move/Transform, transform overlays and snapping, selected-pixel transforms, vector/path commands, masks, duplicate/group/merge, Smart Layer source commands, Fit Canvas to Selected Layers and Export Selected SVG now use hierarchy-safe owning-layer roots.
- Live Filter rows use the same owner-resolution rule for canvas/layer commands, matching the established presentation-child model.
- Delete and Move Up/Down remain sub-item scoped: a selected Layer Effect is removed/reordered rather than deleting or reordering its owner layer.
- Whole-layer, selected-pixel and text-box transform commits preserve the highlighted Layer Effect/Live Filter row, so repeated canvas moves do not kick the Inspector back to the parent layer.
- No persistence/schema changes.

## 0.14.0m.1 Layer Effect Immediate Refresh Fix

- Adding a renderable Layer Effect now schedules the canvas composite immediately instead of waiting for a later property/canvas refresh.
- Removing and reordering Layer Effects use the same immediate preview/thumbnail invalidation path.
- Layer Effect persistence schemas remain unchanged.

## 0.14.0m Full Workflow Integration and Hardening

- Linked-source save propagation is now selective across currently open documents while remaining transitive through graphs such as `A → B → C`. Runtime resolved document-identity closures skip unrelated linked graphs without changing persisted source identity or copy semantics.
- Quick Export and Production Export refresh external linked Smart Sources immediately before the accepted export is snapshotted, preventing stale linked presentations from entering flattened or queued outputs. Cancelling an export does not mutate document/history state.
- Structural Undo/Redo and Smart Source registry adoption now rebuild document-level missing-link diagnostics, so warning state follows the restored registry instead of becoming stale.
- Save/Save As similarly rebuilds linked-source diagnostics after path rebasing.
- Targeted linked refresh preserves warnings from unrelated unresolved links while invalidating only source branches whose resolved revision actually changed.
- Added cross-feature regression coverage for 16-bit embedded Smart transforms + Live Filters + filter masks + Layer Effects + layer masks + opacity/blending round-tripping as one workflow.
- Updated stale current-format test expectations introduced by the project-27 transition in 0.14.0l.
- No persistence bump is required: public `.vfxphoto` format remains **27**, Smart Source descriptor schema **3**, embedded Smart document schema **10**, private Hot/Warm/Cold snapshot format **28**, and Layer Effect schema **4**.

0.14.0m is now the milestone-wide testing gate. **0.15.0 — Camera and Lens Corrections remains blocked until 0.14.0 has passed Fedora/manual hardening and any regressions found here are fixed.**

Architecture/hardening notes: `docs/FULL_WORKFLOW_HARDENING_0.14.0m.md`. Fedora regression plan: `docs/TEST_PLAN.md`.

## Windows builds and releases

Windows x64 binaries are built on GitHub rather than on the Fedora development machine. Each public GitHub Release can contain a normal per-user Setup installer, a portable ZIP and matching SHA-256 checksums. The Windows build includes the native Qt application, pinned wgpu-native backend and OpenColorIO support; users do not need a compiler or Qt installation.

Maintainer publication follows the same one-command workflow as VFX Texture Lab:

```bash
./tools/publish_windows_release.sh --yes
```

That command commits/pushes the current release, starts the clean Windows GitHub Actions build, waits for tests and installer smoke checks, derives patch notes from `CHANGELOG.md`, verifies the release assets and publishes the result. See [`docs/WINDOWS_RELEASES.md`](docs/WINDOWS_RELEASES.md) for the complete workflow and first-time GitHub CLI setup.

## 0.14.0k.2 Bevel & Emboss Fedora Build Fix 2

This is a build-only follow-up to 0.14.0k. It includes the 0.14.0k.1 persistence/cache-key compile fixes and additionally fixes Qt 6 `qsizetype`/`int` mismatches in the Layer Effect and Live Filter reorder paths. Rendering and persisted schemas are unchanged.

## 0.14.0k Bevel & Emboss

- **Bevel & Emboss** is generated non-destructively from the layer's full-precision, mask-aware effective coverage using a signed-distance-derived height field rather than offset-shadow approximation.
- Styles: **Inner Bevel**, **Outer Bevel**, **Emboss**, and **Pillow Emboss**.
- Direction: **Up** or **Down**.
- Editable **Depth**, **Size**, **Soften**, light **Angle** and **Altitude**.
- Independent Highlight and Shadow colours, blend modes and opacities.
- Finite-difference surface normals are lit from the authored angle/altitude and emitted as independent highlight/shadow passes, preserving real backdrop blend semantics.
- Exact 8-bit/16-bit CPU-reference generation is shared with native hierarchy composition; WebGPU can retain/composite the generated passes without substituting a cheaper bevel algorithm.
- Bevel participates in the existing bounded Layer Effect cache, effect-expanded tile dependencies, off-canvas rendering, masks, Smart Layers, Live Filters, Undo and export/residency paths.
- Large full-resolution CPU renders stream Bevel in bounded tiles so signed-distance/height/highlight/shadow working data does not multiply into several full-canvas buffers.
- Layer Effect schema is now **4**. Public `.vfxphoto` format is **26**, embedded Smart document schema is **9**, and private Hot/Warm/Cold snapshot format is **27**.
- Rich gloss/contour libraries, textures/patterns and more specialised styles remain appropriate for **0.17.0 — Filters and Effects Expansion**.

The semantic order remains:

`Layer content / Smart Source + Transform + Live Filters` → `Layer Effects` → `Layer Mask` → `Opacity / Blend`

See `docs/LAYER_EFFECT_BEVEL_EMBOSS_0.14.0k.md` and `docs/TEST_PLAN.md`.
