# 0.14.0c — Smart Layer Edit Contents and Source Dependencies

## Scope

0.14.0c turns the embedded Smart Source payload introduced in 0.14.0b into an editable production workflow. It deliberately does **not** implement source-backed repeated transform resampling (0.14.0d), the final per-tile Smart renderer/cache architecture (0.14.0e), Live Filters, Layer Effects, or externally linked sources (0.14.0l).

## Source editor sessions

An embedded Smart Source is opened as a normal `PhotoDocument` inside a normal `DocumentSession`. The session stores an explicit binding containing:

- immediate owner session ID;
- Smart Source ID;
- source display name;
- baseline revision for every Smart Source present when the branch was opened.

The source editor otherwise participates in the normal document strip, renderer-session isolation, Undo/Redo and Hot/Warm/Cold residency. Its canvas, bit depth, resolution, colour model, colour-management state and embedded layer tree come from the authoritative Smart Source payload.

A source editor is a **branch edit**. Ordinary edits mutate only the source editor's copy. Saving is the transaction boundary that attempts to merge the branch into its immediate owner.

## Commit and dependency propagation

Save first verifies that the owner target source still has the revision recorded when Edit Contents was opened. A stale owner therefore cannot be overwritten silently.

Nested Smart Sources created or edited inside the branch are adopted into a prepared owner-registry copy. The edited target source is then re-encoded from its current layer tree and document settings. Dependencies are recomputed from the Smart references actually present in that tree.

Revision changes use the existing dependency graph. The directly edited source advances, then each transitive dependant advances. Embedded presentations are refreshed dependency-first so a parent source never renders against a stale nested presentation.

Only the transitive affected subgraph is refreshed. Unrelated Smart Sources retain their revision/cache identity.

The complete prepared registry and owner layer tree are validated and presentation-bound before either replaces live owner state. A stale revision, invalid Smart reference, unsafe colour-space boundary or circular graph therefore leaves the owner unchanged.

## Circular dependency protection

A dependency such as:

```text
A → B → C → A
```

is rejected during the prepared-registry transaction. This includes cycles introduced by editing the contents of a nested source and inserting a Smart reference back to one of its dependants.

## Undo and inactive owners

A successful Save becomes one **Update Smart Layer Contents** structural history item in the immediate owner. The history state includes the Smart Source registry, source revisions, root Smart observed revisions, layer tree and document structural state.

The owner does not have to be the active tab. If it is Cold, it is restored to Warm residency directly without stealing focus from the source editor. Inactive-owner Undo/Redo restores its structural state and retires its renderer identity so obsolete tile work cannot be reused.

## Nested Edit Contents

A source editor can itself contain Smart Layers and open their contents. Such a nested editor is owned by the source-editor session immediately above it. Saving the nested editor updates that branch; saving the parent source editor later commits the branch upward.

Closing an owner recursively asks its direct source-editor children to Save/Discard/Cancel first. This prevents an orphan Edit Contents tab with no authoritative owner to commit into.

## Precision migration

Embedded document schema 2 adds an explicit `bitDepth` field. The source editor therefore reconstructs the authoritative canvas as either RGBA8888 or RGBA64 instead of inferring precision from unrelated parent state.

- `.vfxphoto` format **19** writes schema 2.
- format **18** schema-1 embedded sources remain readable; their precision is inferred from the accepted 0.14.0b presentation cache.
- schema-2 precision metadata inside a pre-v19 project is rejected.
- private session snapshot format **20** stores schema-2 state plus source-editor bindings.
- schema-2 precision metadata inside a pre-v20 session envelope is rejected.

## Colour-management boundary

The authoritative embedded source remains tagged/stored in its own document working space. Rendering its presentation does not rewrite that authoritative representation into the parent working space.

At the containing-document boundary, the derived Smart presentation is converted from the source QImage working-space tag into the containing document's working-space tag. Equal spaces share the source presentation; tagged↔untagged ambiguity is rejected instead of being silently reinterpreted. Conversion preserves the original QImage precision.

Nested source presentations are bound into the embedded source's working space before that source is rendered. This gives each containment boundary one explicit working-to-working conversion rather than repeatedly transforming authoritative source pixels.

## Intentional interim rendering

0.14.0c continues to use the bounded whole-source presentation cache established by 0.14.0b. That cache is derived and revision-keyed; it is not authoritative source storage. 0.14.0e remains responsible for replacing this interim cache with source-tile requests, transform-aware dirty propagation, intermediate tile caches and bounded GPU residency.
