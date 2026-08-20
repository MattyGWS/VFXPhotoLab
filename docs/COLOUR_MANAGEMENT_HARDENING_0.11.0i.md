# 0.11.0i — Persistence, Residency and Final Hardening

## Scope

This stage closes the 0.11.0 Colour Management Foundation milestone. It does not add a new colour transform or change any persisted schema. Its job is to prove that the state introduced by 0.11.0a–h remains deterministic through projects, private residency snapshots, missing external dependencies, multiple open documents and legacy migrations.

## Stable schemas

- `.vfxphoto` project format: 15
- private Hot/Warm/Cold snapshot format: 16
- document colour-state JSON: 4
- adjustment schema: 10
- vector schema: 7

Keeping these versions stable is deliberate: 0.11.0i validates and audits existing state rather than rewriting it.

## Project repair versus runtime resources

Project load now has two distinct result classes.

**Project-data repairs** cover malformed optional layer, mask or selection data that can be safely discarded or repaired. These mark the opened project modified so the user can save a clean copy.

**Colour-resource issues** cover files/configurations referenced by an otherwise intact colour state. They never mark the project modified by themselves, never create Undo history and never choose an alternative resource.

The open dialog consolidates both categories, but explicitly states which repairs dirtied the document and which dependencies merely need attention.

## External ICC authority

An external ICC descriptor persists:

- the original canonical source path;
- an exact 32-byte SHA-256 fingerprint;
- an embedded ICC copy when supported;
- the display name and semantic role.

The embedded copy is authoritative for reproducibility. If the source path later disappears, becomes unreadable, exceeds the safety limit, changes fingerprint or no longer parses as a supported ICC profile, VFX Photo Lab continues using the embedded copy and reports that explicit relinking is required to adopt a different file. It does not reinterpret document pixels from the changed file.

A blocking issue is reported only when a required external ICC reference has no valid embedded copy.

## OpenColorIO identity

A saved OCIO configuration is resolved only through its recorded source/identifier and must match its recorded fingerprint. The audit also checks referenced input/working/proof/output colour spaces and any saved Display/View/Look selection.

When resolution or validation fails:

- no alternate configuration is loaded;
- no similarly named colour space, display, view or look is chosen;
- saved identifiers remain intact for explicit relinking;
- only operations requiring the unavailable transform are blocked/fallbacked.

## Residency

Runtime resource warnings are not written into private snapshots. After a Cold document is restored, the current filesystem and OCIO environment are audited again. This catches a profile/configuration that was removed or changed while the document was evicted while retaining the exact saved colour state and embedded data.

Hot and Warm documents remain in memory and isolated per document. Switching documents cannot copy proof, Display/View, output-profile or resource-warning state between sessions.

## Metadata and cache identity

Display/proof and output defaults are save-worthy metadata but are not authoritative pixel-processing operations. Reapplying identical metadata is a no-op: it does not dirty the project, change `DocumentColourState::revision`, replace source/layer images or invalidate tile, histogram, thumbnail, presentation or export caches.

Actual metadata changes still mark the project modified so they persist, but do not create an Undo command.

## Validation hardening

- External ICC fingerprints must be exact SHA-256 length and any embedded ICC bytes must hash to that identity.
- Automatic/manual monitor ICC reads retain the 16 MiB profile safety limit and complete-read check.
- Embedded project source-image base64 uses strict decoding and rejects invalid characters/truncation.
- Existing bounded image, tree, guide, vector, profile and snapshot-field limits remain authoritative.
- Corrupt or incompatible snapshots do not replace the resident document.
- Monitor/proof/display transforms remain presentation-only.
- Output conversion and blue-noise dithering remain export-only.

## Compatibility

Projects from versions 1–14 still load through the existing Legacy V1 migration path without automatic colour conversion or presentation activation. Schema-1/2/3 colour states retain the schema-4 legacy presentation gate. Existing version-15 ICC-only and OCIO projects retain their saved pixels, descriptors and fingerprints.

No missing profile or configuration is silently replaced, and opening an intact project with an unavailable external dependency does not itself change the project.
