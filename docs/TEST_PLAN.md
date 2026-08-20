# Fedora KDE test plan — 0.14.0m Full Workflow Integration and Hardening

## 0.14.0m.2 focused owner-selection regression

1. Add a visible Layer Effect (Drop Shadow is ideal) to a vector/text/raster layer and click the individual effect row in Layers.
2. Choose Move/Transform. The owner layer must show its transform overlay immediately even though the effect row remains highlighted.
3. Drag the owner around several times. The layer and its effect must move together, and the Inspector must remain on the same effect after each committed move.
4. Scale/rotate the owner and verify the same focus preservation. Repeat on a text layer resize handle and, on a raster layer with an active selection, Selected Pixels transform.
5. With the effect row still selected, verify snapping/transform numeric controls refer to the owner layer.
6. Verify Delete removes the selected effect only, while Move Effect Up/Down reorders the effect only.
7. Verify Duplicate duplicates the owning layer, and Add Mask targets the owning layer.
8. On a vector layer effect row, verify Direct Selection / Convert Shape to Path can still target the owner vector layer.
9. Repeat the transform test with a Live Filter row on a Smart Layer; canvas transform targets the Smart Layer while filter controls remain focused.
10. Multi-select a group plus a descendant effect/filter row and transform: the descendant must not be transformed twice; only the hierarchy root is transformed.


1. Preserve the existing `build/` and `third_party/` directories, overwrite the source tree, and run `./run.sh`.
2. Confirm **Help → About** reports **VFX Photo Lab 0.14.0m.2** and the current `.vfxphoto` format is still **27**.
3. Open representative 0.14.0k.2 and 0.14.0l projects before saving. Confirm embedded Smart Layers, Linked Smart Layers, Live Filters and all existing `fx` retain their appearance; saving upgrades only where the existing migration rules require it.
4. Build an embedded Smart Layer containing raster/vector/text/group content. Transform it repeatedly, edit its contents, save the editor, Undo/Redo the owner update, and verify source pixels are never cumulatively resampled.
5. Copy an ordinary embedded Smart Layer into an unrelated document. Confirm it becomes an independent embedded source rather than an accidental cross-document link.
6. Place a Linked Smart Layer and give the instance a transform, multiple Live Filters, a Live Filter mask, Drop Shadow/Glow/Stroke/Overlay/Bevel effects, a layer mask, opacity and a non-Normal blend mode. Save/reopen and confirm all state remains instance-owned.
7. Build `A → B → C`, keep all three open, edit/save C, and confirm both B and A update. Keep a separate unrelated linked graph open and watch for needless refresh/re-render activity there; it should remain untouched by the C save.
8. Repeat step 7 with A or B Warm/Cold. Restoration must resolve current source revisions without resurrecting stale tiles/presentations.
9. Create a missing linked source, then Relink it and Undo the operation. The missing-link warning/status must return with the restored source registry. Redo must clear it again.
10. Test Replace Source, Relink Source, Embed Linked Source and nested linked Smart Layers. Identity mismatch must never silently substitute another source, and `A → B → C → A` authoring must remain rejected.
11. Modify a linked source externally/on another open document, then choose **Quick Export** without manually refreshing the owner first. The exported result must reflect the current source. Cancel a subsequent export dialog and confirm cancelling does not create an extra linked-refresh history step.
12. Repeat step 11 with **Production Export** and the queue. The queued immutable snapshot must contain the current linked source revision, not the presentation that happened to be cached before opening the export dialog.
13. With a link genuinely unavailable, export using its last valid retained presentation. Confirm the warning is visible and no similarly named `.vfxphoto` is substituted.
14. Exercise Smart Transform + Live Filters + Live Filter masks + all core `fx` + layer masks together on both 8-bit and 16-bit documents. Look specifically for order changes, mask-space errors, hidden-RGB damage and stale tiles.
15. Test Raster, Vector, Text and Smart `fx` owners inside isolated groups and Pass Through groups, including nested groups. Toggle group/layer visibility, masks, opacity and blend modes while panning/zooming.
16. Recheck Drop Shadow, Inner Shadow, Outer/Inner Glow, Stroke (Inside/Centre/Outside), Colour Overlay, linear/radial Gradient Overlay and all Bevel & Emboss styles/directions at image edges and off-canvas bounds.
17. Exercise editable RGBA channels, zero-alpha hidden RGB, masks and selections underneath Smart/filtered/effected stacks. Hidden RGB must survive even when the visible alpha contribution is zero.
18. Test mixed 8/16-bit source/owner combinations and ICC working spaces. Repeat with OCIO/ACES processing where available, display management and soft proofing enabled. Authoritative linked source pixels must not be repeatedly converted merely because the owner refreshes.
19. Compare native tiled WebGPU with honest CPU fallback on transformed Smart Layers with spatial Live Filters and large-radius `fx`. Look for seams, halo clipping, stale tiles or different effect ordering.
20. Paint rapid long strokes beneath a deep Smart + Live Filter + `fx` stack. Check latency during the stroke and commit, and verify unrelated Smart/filter/effect cache branches do not visibly recompute on every dab.
21. Stress large images and several open documents through Hot → Warm → Cold → restore. Watch memory growth, cache eviction, source/editor restoration, thumbnails and document-strip responsiveness.
22. Exercise Undo/Redo through long mixed sequences: painting below the stack, Smart transforms, filter edits/mask edits, effect edits/reorder/toggles, linked refreshes, Relink/Replace/Embed, group changes and colour-state changes.
23. Test recovery/session persistence with embedded Smart editors and linked owners open. Restore the session and verify editor bindings, source identities, link warnings and Hot/Warm/Cold state are coherent.
24. Run Quick Export, Production multi-output export and queued export across PNG/JPEG/TGA/TIFF/WebP paths used in normal testing, including 16-bit-capable outputs and colour-managed output profiles.
25. Recheck the QoL/visual bugs collected during 0.14.0h–l testing. Record anything still wrong now; 0.14.0m is the intended broad fix gate rather than deferring those issues into 0.15.0.
26. Close/reopen documents repeatedly and exit the application after a heavy multi-document session. Report any Fedora terminal warning, crash report, stale helper process or shutdown corruption.
27. If the build succeeds, run `ctest --test-dir build --output-on-failure` and include any failing test names/output with the Fedora report.

0.14.0 has no planned implementation stage after this one. Any failures from this checklist should be fixed as 0.14.0m follow-ups before the milestone is accepted and before 0.15.0 begins.
