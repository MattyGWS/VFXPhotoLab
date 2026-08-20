# 0.12.0a — Unified Preset Architecture

## Scope

This stage creates the reusable preset foundation for adjustment presets, vector appearance presets and later production-export profiles. It deliberately does not add the full preset browser, production-export dialog, multi-output renderer or queue.

## Stage map for 0.12.0

1. **0.12.0a — Unified Preset Architecture**: shared envelope, stable IDs, storage, validation, legacy adapters and deterministic tests.
2. **0.12.0b — Preset Management UX**: searchable/category/favourite/recent browser and complete user CRUD/import/export controls.
3. **0.12.0c — Export Profiles and Naming Templates**: versioned output definitions, colour/bit-depth/Alpha settings and validated naming tokens.
4. **0.12.0d — Production Multi-Output Export**: one document to multiple independently resized/named outputs while preserving simple quick export.
5. **0.12.0e — Export Queue Foundation**: responsive progress, cooperative cancellation, failure isolation, collision policies and atomic output publication.
6. **0.12.0f — Recovery and Session Persistence**: recoverable job descriptions plus document-specific production-export state through project and Hot/Warm/Cold boundaries where appropriate.
7. **0.12.0g — Integration and Hardening**: deterministic parity, malformed-state, multi-document, shutdown and large-image stress coverage.

Folder-wide batch processing and automation remain reserved for 0.19.0.

## Envelope

New preset files use:

```json
{
  "format": "vfxphotolab-preset",
  "version": 2,
  "kind": "adjustment | vector-appearance | export-profile",
  "metadata": {
    "id": "stable identifier",
    "name": "display name",
    "category": "category",
    "tags": ["tag"],
    "favourite": false,
    "builtIn": false,
    "createdUtcMs": 0,
    "modifiedUtcMs": 0,
    "lastUsedUtcMs": 0,
    "useCount": 0
  },
  "payload": {}
}
```

The generic core validates only envelope and metadata invariants. Feature stores validate payload semantics so export-profile evolution cannot weaken adjustment or vector validation.

## Stable identity

Built-ins are generated in code and receive deterministic IDs derived from kind, scope and name. User presets receive UUID-backed IDs. Rename and update preserve the ID. Duplicate creates a new ID. Export preserves identity; importing an already-present ID is rejected rather than silently overwriting it. Name collisions on import receive a visible numeric suffix while retaining the imported ID.

Legacy files have no stored ID. Their adapter derives a deterministic `legacy.*` ID from kind, scope, name and canonical payload. The ID is retained when that preset is explicitly migrated.

## Storage

`QStandardPaths::AppDataLocation` supplies the platform root:

```text
<persistent app data>/presets/adjustment/<adjustment-type>/
<persistent app data>/presets/vector-appearance/
<persistent app data>/presets/export-profile/        (future stage)
```

On Linux this follows the XDG app-data location; on Windows it follows the application data location selected by Qt. The existing home-directory fallback remains for environments where Qt cannot resolve app data.

## Legacy safety

The old adjustment location remains readable:

```text
<persistent app data>/adjustment-presets/<adjustment-type>/
```

The old vector version-1 files remain readable in the vector preset directory. Launching the application, listing presets or opening the manager does not rewrite them. Save, rename, update, favourite or successful recent-use recording is an explicit mutation and migrates only that preset; applying a user preset therefore migrates it when recent-use metadata is recorded. Migration commits the new file first and removes the old file second; removal failure deletes the new file so both representations are not left active.

## Validation and failure isolation

- Adjustment presets retain a 32 MiB limit so large embedded LUT payloads remain supported without unbounded reads.
- Vector appearance presets retain a 128 KiB limit.
- IDs, names, categories, tags, timestamps and counters are bounded.
- Kind mismatches, unsupported versions, malformed JSON, invalid payloads, duplicate IDs and duplicate user names are skipped independently and reported as warnings.
- Delete/update operations require the path to be directly inside the expected managed directory.
- Writes use `QSaveFile` for atomic replacement.

## 0.12.0b consumer

The shared management UI implemented in 0.12.0b consumes this contract without changing the version-2 envelope. Built-in favourite/recent values use a separate stable-ID-keyed usage sidecar; user preset metadata remains in the envelope. See `PRESET_MANAGEMENT_UX_0.12.0b.md`.

## Compatibility

No `.vfxphoto`, private residency, colour-state, adjustment or vector schema changes occur. Existing rendering and export code is untouched. LUT tables remain embedded in `AdjustmentData`; generic LUTs, Tony McMapface and AgX do not acquire a new interpretation.
