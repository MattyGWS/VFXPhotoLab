# 0.12.0f — Export Queue Recovery and Session Persistence

## Scope

0.12.0f makes the 0.12.0e application queue recoverable across crashes and orderly restarts. It does not add folder discovery, unattended automation or batch scheduling; those remain 0.19.0.

## Private recovery ownership

Each accepted unfinished job is written through `QSaveFile` to a versioned private file under:

- Linux: the Qt `AppLocalDataLocation` for `VFX Suite/VFX Photo Lab`, below `export-queue/recovery`;
- Windows: the equivalent local application-data directory, below the same relative path.

The environment variable `VFXPHOTOLAB_EXPORT_QUEUE_RECOVERY_ROOT` is accepted only as a deterministic test override.

Recovery files are not stored in `.vfxphoto`, document Undo history, colour state, preset directories or Hot/Warm/Cold snapshots. Project format remains 15 and private residency schema remains 16.

## Captured contract

A recovery file contains:

- stable job ID and creation time;
- job/document labels;
- the immutable source image in compressed exact straight-RGBA storage;
- the complete versioned layer tree, including masks, raster/vector/text/adjustment data and embedded LUT payloads;
- document colour-management state and processing-compatibility contract;
- the executable enabled outputs from the production-export plan, including profile snapshots, naming templates, resize settings and collision policy. Disabled editor drafts are intentionally omitted because they can never execute.

The source image uses raw RGBA8 or RGBA16 rows before compression, preserving hidden RGB beneath zero Alpha. Layer payloads use the same validated layer JSON contract as project persistence.

## Startup recovery

The application scans at most 128 bounded `*.vfxqueue.json` files and restores at most 16 valid unfinished jobs. Valid jobs are restored as **Recovered**. Recovered is non-terminal but never selected by the serial worker, so startup cannot write files.

Malformed, oversized, wrong-kind, wrong-schema, mismatched-processing or compatibility-repaired files are renamed with an `.invalid` suffix and reported through bounded startup warnings. They are not interpreted or silently deleted.

## Explicit resume

**Resume Recovered** performs a fresh `resolveProductionExportPlan` pass against the saved colour state and the current filesystem. It rechecks:

- output-profile payload validity;
- filename templates and resolved paths;
- writer and bit-depth support;
- resize dimensions and surface safety limits;
- duplicate destinations;
- current collision state.

Ask-before-replace jobs require a new explicit confirmation for files that currently exist. Skip-existing and Auto-Rename are recalculated at resume and checked again immediately before atomic output publication.

Recovery is replay-based rather than a mid-output checkpoint. Files already completed before interruption remain intact; the chosen collision policy determines what happens when the recovered description is resumed.

## Shutdown choices

Closing with unfinished jobs offers three choices:

1. **Preserve Jobs and Quit** — cooperatively stop the active worker, keep private recovery files and restore all unfinished records as Recovered next launch.
2. **Cancel Jobs and Quit** — cancel unfinished jobs and delete their recovery files.
3. **Keep Application Open** — make no queue changes.

If a later unsaved-document prompt cancels the quit after preservation, the controller leaves shutdown mode and the preserved records remain available for explicit review/resume.

## Cleanup and bounds

Recovery files are deleted when a job reaches a terminal Completed, Completed with issues, Failed or Cancelled state, or is explicitly discarded. A recovered job that fails preflight remains available for review and retry until the user cancels it. The existing limits remain:

- 16 unfinished/recoverable jobs;
- 128 recovery files scanned at startup;
- 128 lightweight in-memory history records;
- 32 outputs per production plan;
- 1 GiB per private recovery file;
- existing layer-tree, image-size, colour-state and profile safety limits.

## Compatibility

No `.vfxphoto`, Hot/Warm/Cold, colour-state, adjustment, vector, preset or export-profile schema changes are introduced. Native tiled GPU rendering, CPU fallback, Alpha-safe hidden RGB, ICC/OCIO/ACES processing, quick export, multi-document isolation and the 0.11.0i.3 shutdown ordering remain authoritative.
