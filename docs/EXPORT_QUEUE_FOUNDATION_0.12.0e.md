# 0.12.0e — Export Queue Foundation

## Scope

This stage moves Production Export from a document-blocking modal operation into a bounded application-level queue. Quick single-image export remains unchanged. Production Export still performs the complete 0.12.0d preflight before a job is accepted, then captures an immutable copy-on-write document snapshot and returns control to the editor.

Queue recovery across application restarts is intentionally deferred to 0.12.0f. Folder discovery, folder-wide batch processing and automation remain 0.19.0.

## Job ownership and isolation

A queued job owns:

- a stable UUID job identifier;
- the document display name and creation timestamp;
- a copy-on-write source image and layer-tree snapshot;
- the document colour state and processing-compatibility contract;
- the resolved production-export plan and per-output profile snapshots;
- progress, state, output counts, warnings and failure details.

No queued job stores a pointer to a live `PhotoDocument` or `DocumentSession`. Editing, switching, evicting or closing the source document cannot redirect a pending job. Snapshot pixel/layer payloads are released immediately after completion, cancellation or failure.

The queue accepts at most 16 unfinished jobs, retains at most 128 lightweight job records and executes exactly one job at a time. Oldest terminal records are pruned when the retained-history cap is reached. This bounds concurrent full-resolution render, resize, colour-conversion, writer and long-running UI-history pressure while still allowing jobs from several documents to wait independently.

## States and controls

The stable in-memory states are:

- Pending
- Running
- Paused
- Completed
- Completed with issues
- Failed
- Cancelled

The modeless **Export Queue** dock shows job state, overall progress, output counts, destination and detailed warnings/errors. It provides:

- Pause Queue / Resume Queue;
- Cancel Job;
- Cancel All;
- Remove terminal job;
- Clear Finished.

Pause is cooperative. It takes effect at safe boundaries before the shared render or between outputs; an already running resize, transform or encoder step is allowed to reach its next safe checkpoint. Cancellation remains responsive inside the existing cancellable render, GPU/CPU resize and colour-conversion contracts.

## Execution contract

Each job preserves the 0.12.0d order:

1. render the captured document once in working RGB;
2. derive one output-size surface at a time;
3. perform that output's colour conversion and encoding preparation;
4. recheck collision policy;
5. publish through the existing atomic write path;
6. release the prepared output and continue.

The queue rechecks skip, ask-before-replace and auto-rename policies both before expensive output preparation and immediately before writing. A late file appearance cannot silently bypass the confirmed collision contract.

One output failure does not stop later independent outputs. Completed files, warnings and errors remain visible in the terminal job record. A cancelled job keeps every output already published atomically.

## Shutdown contract

Closing the application with unfinished queue work asks whether to cancel the active and pending jobs. When confirmed, the window remains alive until cooperative cancellation has reached the queue's idle boundary, then normal project-save prompts and shutdown continue. `MainWindow` also explicitly drains the controller before child teardown as a final memory-safety guard.

## Persistence boundary

0.12.0e does not serialize queue records into:

- `.vfxphoto` projects;
- Hot/Warm/Cold snapshots;
- preset or export-profile files;
- document colour state;
- Undo/history.

Cross-session job descriptions, recovery and appropriate document/session state belong to 0.12.0f.

## Compatibility

Project format remains 15, Hot/Warm/Cold schema remains 16, colour-state schema remains 4, adjustment schema remains 10 and vector schema remains 7. The queue does not change quick-export pixels, production-export profile interpretation, ICC/OCIO/ACES transforms, hidden-RGB behaviour, Alpha handling, dithering, native tiled GPU paths or CPU references.
