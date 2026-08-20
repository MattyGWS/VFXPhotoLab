# 0.14.0l — Linked Smart Layers, Replace and Relink

## Purpose

0.14.0l makes cross-document Smart Source sharing explicit. Ordinary embedded Smart Layers retain the existing copy semantics: moving/copying an embedded Smart Layer into an unrelated document must not silently establish a cross-document relationship. A **Linked Smart Layer** is the deliberate mechanism for sharing an external `.vfxphoto` source.

## Persistent source contract

Every public project-format-27 document stores a `documentIdentity` UUID. A linked Smart Source descriptor stores three independent pieces of state:
When a pre-format-27 project is first opened, its migration identity is derived from the project bytes when available rather than its pathname; a pure filesystem move therefore does not defeat Relink before the project is first saved in 0.14.0l. The next save persists that UUID explicitly.


- `linkedPath`: relocatable path metadata, persisted relative to the owning project when possible.
- `linkedDocumentId`: persistent source identity, independent of path.
- `linkedFingerprint`: resolved authoritative revision fingerprint.

The resolved fingerprint hashes the direct `.vfxphoto` bytes plus the resolved fingerprints/identities of nested linked Smart Sources. This means `A → B → C` notices a saved change to `C` without requiring `B.vfxphoto` to be rewritten simply to propagate a revision.

Runtime availability and warning text are deliberately not persisted. A missing/corrupt/identity-mismatched file retains the last valid presentation cache when one exists and is marked unavailable; no different source is selected automatically.

## Source management semantics

### Place Linked Smart Layer

Loads an external `.vfxphoto`, records its persistent identity/path/fingerprint, renders its current document canvas in the source document's own authoritative working colour space, and creates a Smart Layer instance referencing that descriptor.

### Relink Source

Changes path metadata only when the selected file resolves to the same persistent `documentIdentity`. If source contents changed while the file was moved, the source/dependent revision branch advances and its presentation is refreshed. If contents did not change, existing Smart/tile caches remain reusable.

### Replace Source

Explicitly adopts a different source identity. The Smart Source identity inside the owner remains the same so all instances sharing that linked descriptor update together, while each layer instance retains its own transform, Live Filters, Layer Effects, mask, opacity and blend mode.

### Embed Linked Source

Imports the linked document's editable root layers and Smart Source graph into the owner and changes the selected source descriptor from `Linked` to `Embedded`. Imported Smart Source UUID collisions are remapped. Nested linked paths are rebased from the external project's directory to the owner project's directory. The outer instance remains unchanged.

The immediate embedded presentation is kept aligned with the linked document's visible canvas so Embed itself does not introduce an appearance jump.

## Edit Contents

For a linked Smart Layer, Edit Contents opens the external `.vfxphoto` as a normal document session, reusing an already-open session for the same canonical path. Identity-mismatched or missing links cannot be opened through the stale path; the user must Relink or Replace explicitly.

Embedded Smart Contents editors have no public project path. Any linked sources in their shared registry are therefore made absolute while editing, then rebased to the owner on commit. After an embedded source Save, the editor registry is rebased back to its absolute editor-safe form so repeated saves do not accidentally resolve relative links against the process working directory.

## Cross-document updates

Saving a normal `.vfxphoto` scans currently open normal document sessions that contain linked sources and refreshes their resolved source fingerprints. Transitive chains are supported because loading an intermediate target resolves its own linked-source fingerprints before the outer fingerprint is calculated.

Only linked descriptors whose resolved fingerprint actually changed (or whose cache is missing) rebuild their root presentation and advance source/dependent revisions. Unchanged links are checked without re-rendering their full presentation. Existing Smart source/tile caches remain content/revision addressed; no global Smart cache flush is performed.

Transient embedded Smart Contents editor branches are not mutated by this cross-document scan because doing so would invalidate their source-edit conflict-detection baseline while they contain unsaved editable state. Their owning normal document is refreshed instead.

## Circular dependency safety

Authoring Place/Replace/Relink/Embed operations run with strict cross-file cycle detection. The canonical project load stack rejects cycles such as:

`A → B → C → A`

before a partial source graph is committed.

Normal opening/restoration of an already-damaged cycle is bounded rather than recursive: the offending link becomes unavailable and retains its last safe cached presentation where possible.

## Colour and precision

The external `.vfxphoto` remains authoritative in its own working colour state and bit depth. The linked source's root document is rendered in that state. Conversion into the parent working space happens through the existing Smart-presentation binding contract. Authoritative source pixels are not repeatedly converted merely because the same linked source is instantiated in multiple differently managed documents.

The established hidden-RGB/alpha-safe, 8-bit/16-bit, ICC, OpenColorIO/ACES, display-management and soft-proofing contracts remain unchanged.

## Residency, recovery and export

Private Hot/Warm/Cold snapshot format 28 stores the public document identity and the linked Smart Source descriptor/cache state. Restoring a current snapshot rechecks links before the session becomes resident, preserving cached presentations for unavailable sources and marking the document dirty if an authoritative linked revision advanced.

Quick Export, Production Export and queued export continue through the normal document layer/render contracts; linked Smart Layers are ordinary Smart presentation inputs at that stage. Missing links export their retained last-valid presentation when one exists rather than substituting another file.

## Save As

Relative link paths are rebased from the old owner path to the new owner path without changing the target they resolve to. Saving an owner project over one of its own linked source files is refused.

## Persistence versions

- Public `.vfxphoto`: **27**
- Smart Source descriptor: **3**
- Embedded Smart document: **10**
- Hot/Warm/Cold snapshot: **28**
- Layer Effect schema: **4** (unchanged)

## Next task

0.14.0m is the final milestone stage: full workflow integration, regression, bugfix/QoL, performance and hardening across Smart Layers, Live Filters and Layer Effects.
