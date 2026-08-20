# 0.14.0m — Full Workflow Integration and Hardening

## Scope

0.14.0m is the final implementation stage of the Smart Layers, Live Filters and Core Layer Effects milestone. It deliberately adds no new persisted editing subsystem. Its job is to make the architecture delivered by 0.14.0a–l behave as one production workflow across history, multi-document residency, recovery, exports and the native tiled renderer.

The semantic order remains:

`Layer content / Smart Source + Transform + Live Filters` → `Layer Effects` → `Layer Mask` → `Opacity / Blend`

Ordinary embedded Smart Layer copies remain independent when moved into unrelated documents. Explicit Linked Smart Layers remain the only cross-document sharing mechanism.

## Selective linked-source propagation

0.14.0l made source fingerprints transitive, but an open-document source save still had to inspect every document containing any linked source. 0.14.0m records a runtime-only, safety-bounded closure of resolved linked document UUIDs on each linked Smart Source. A save notification can therefore ask whether an owner depends directly or transitively on the saved document before loading or rendering that branch.

For `A → B → C`, A's resolved closure contains both B and C, so saving C still refreshes A even when B's project bytes were not rewritten. A separate `D → E` graph is skipped. The closure is never serialized; project identity, explicit paths and resolved fingerprints remain the persisted source of truth, and session/project restoration resolves the graph again.

A targeted refresh recomputes the complete document warning list after the operation. This is important when one source is being refreshed while another source is already missing: the unrelated warning must not disappear simply because it was outside the targeted branch.

## History and warning-state restoration

Missing-link availability and diagnostic text are runtime metadata, but the document-level warning list must agree with whichever Smart Source registry is currently active. Structural Undo/Redo and Smart Source registry adoption now rebuild that list after restoring the registry. Save/Save As does the same after relative linked paths are rebased.

This prevents states such as “Relink fixed the warning, Undo restored the broken source, but the document still reported no missing links.”

## Export freshness

Quick Export and Production Export now resolve linked Smart Sources immediately before an accepted export is snapshotted. If source contents changed on disk, the same normal Smart Source revision/invalidation path updates the canvas/history state first; the exporter does not perform a hidden one-off render with semantics the document cannot reproduce.

If a link is unavailable but has a valid retained presentation cache, normal 0.14.0l missing-link semantics remain in force and the user receives the warning. If refresh cannot be completed safely, export is aborted rather than silently substituting another source or exporting a knowingly stale resolvable link.

The preflight occurs after export settings are accepted, so cancelling export dialogs does not mutate document or Undo history.

## Cross-feature regression contract

0.14.0m extends core coverage across subsystem boundaries, including:

- transitive versus unrelated linked-source save targeting;
- runtime dependency-closure non-persistence;
- missing-link warning restoration through structural history;
- current project-format expectations after the 0.14.0l format-27 migration;
- a 16-bit embedded Smart instance combining a non-destructive transform, multiple Live Filters, a Live Filter mask, Drop Shadow, Bevel & Emboss, a layer mask, opacity and blend mode, then save/reopen/render equivalence.

The existing dedicated suites remain authoritative for individual Smart Source, transform, Live Filter, Layer Effect, tiled cache, residency, recovery, colour-management and export primitives.

## Persistence

0.14.0m intentionally requires no persisted schema bump:

- `.vfxphoto` project format: **27**
- Smart Source descriptor schema: **3**
- Embedded Smart document schema: **10**
- Hot/Warm/Cold snapshot format: **28**
- Layer Effect schema: **4**

## Milestone gate

There are no additional planned 0.14.0 implementation stages after 0.14.0m. The remaining milestone work is the Fedora/manual regression, performance and QoL pass and any fixes it reveals. 0.15.0 — Camera and Lens Corrections must not begin until that gate is accepted.
