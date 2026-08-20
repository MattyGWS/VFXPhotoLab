# 0.11.0a — Colour-State Architecture and Legacy Protection

## Contract

This stage introduces colour-management identity and persistence only. It does **not** convert pixels, activate display management, alter adjustment mathematics or reinterpret LUTs.

Every document now carries an explicit `DocumentColourState` containing:

- input profile descriptor;
- working-space descriptor;
- display-transform descriptor;
- soft-proofing settings;
- default output settings;
- processing compatibility (`LegacyV1` or `ManagedV1`);
- a monotonic cache revision.

A `ColourSpaceDescriptor` can represent untagged data, a Qt built-in RGB space, embedded ICC bytes, a linked external ICC resource or a future OCIO config/space selection. Semantic SHA-256 fingerprints are independent of cosmetic display names and are used by the transform/cache architecture.

## Compatibility

Projects written by versions 1–14 load as `LegacyV1`. The migration copies the established document colour-space interpretation into the explicit input/working/output descriptors but performs no pixel conversion. Display management and proofing remain disabled. Saving writes project version 15 and retains the legacy flag.

New/imported documents use `ManagedV1` metadata so later stages can opt into corrected processing, but 0.11.0a still follows the pre-existing renderer exactly.

## Persistence and residency

Public project version 15 stores the colour state as bounded JSON with an integrity fingerprint. Private session snapshots advance to version 16 and store the same state. Snapshot versions 2–15 remain readable and migrate to `LegacyV1`.

## Cache identity

The colour-state revision is included in:

- tiled composite cache revisions;
- interactive/progressive preview requests and publication guards;
- histogram keys;
- layer-thumbnail keys;
- document-thumbnail publication guards.

This has no visual effect today, but prevents future Assign/Convert/display changes from reusing stale results.

## Transform service

`ColourTransformService` is a thread-safe cache keyed by semantic source/destination descriptors, transform purpose, rendering intent and black-point-compensation request. In 0.11.0a it exposes Qt-supported ICC transforms only; later stages can add Little CMS and OCIO backends behind the same contract.

## Explicitly unchanged

- source/layer pixel values;
- existing encoded/linear/raw LUT contracts;
- Tony McMapface and AgX pipelines;
- CPU and WGSL adjustment mathematics;
- display presentation;
- export conversion;
- Alpha and hidden RGB;
- project tool behaviour.
