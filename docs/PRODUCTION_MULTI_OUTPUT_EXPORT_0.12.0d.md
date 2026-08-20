# 0.12.0d — Production Multi-Output Export

## Scope

This stage adds production export for several outputs from one open document. Its plan/value types are now consumed by the in-session queue added in 0.12.0e; recoverable jobs are added in 0.12.0f; folder-wide batch processing and automation remain 0.19.0.

The existing **File → Export Flattened Image…** action remains the approachable single-output path. **File → Production Export…** (`Ctrl+Shift+E`) opens the advanced workflow.

## Job description

A `ProductionExportPlan` captures the output directory, document name/size/working-space label, one deterministic naming timestamp, collision policy and ordered output list. Every enabled output stores:

- a stable job-local output ID;
- the export profile's stable ID and display name;
- a complete `ExportProfileData` snapshot;
- an independently editable naming template;
- resize mode, dimensions/aspect choice and resampling method.

The snapshot is authoritative for that dialog. A library profile may be renamed, updated or deleted without silently changing an already configured output. Reapplying an available profile deliberately refreshes the snapshot.

## Resize contract

Outputs may use:

- original document size;
- fit inside exact maximum dimensions while preserving aspect;
- exact width and height without preserving aspect;
- a target long edge;
- a percentage of the document size.

Explicit resized dimensions are limited to 32,768 pixels per axis and every resolved output, including original-size output, is checked against a safe RGBA64 surface byte limit before rendering. Nearest and bilinear resampling prefer the existing native tiled GPU path. Bicubic, Lanczos 3, area and all GPU fallbacks use the existing straight-RGBA CPU reference. Resizing occurs before output colour conversion, in the document working-RGB contract, and preserves hidden RGB semantics.

## Preflight

The complete enabled plan must resolve before the shared render starts. Preflight validates:

- output directory existence and writability;
- at least one enabled output and no more than 32 configured outputs in total;
- stable unique output identifiers;
- export-profile payload and document colour compatibility;
- resize parameters and safe image size;
- portable filename-template resolution using the final output dimensions;
- image-writer availability;
- duplicate output destinations and existing-file policy.

The resolver publishes no partial result when any output is invalid.

## Collision policies

- **Ask before replacing** lists existing outputs before rendering. A file that appears only after confirmation is not overwritten.
- **Replace existing files** permits atomic replacement.
- **Skip existing files** marks pre-existing outputs and rechecks at execution time.
- **Auto-rename new files** deterministically adds `-2`, `-3`, and so on while avoiding existing and job-reserved paths; execution rechecks races.

All final writes retain the existing atomic publication boundary. A failed output does not remove or roll back files completed earlier in the job.

## Rendering, progress and cancellation

The document is rendered once through `ImageProcessor::renderPreservingHiddenRgb()`. Each output is then resized if needed, prepared by the existing colour-managed `prepareImageExport()` path and immediately written. Prepared images are released between outputs rather than retained for the whole job.

The modal progress surface reports the shared render and each output. Cancellation uses the existing cooperative atomic token throughout rendering, resampling and export preparation. Completed files remain intact after cancellation. Non-fatal encoding/profile warnings are collected per completed file and surfaced in the final job summary rather than being silently discarded.

## Compatibility boundary

0.12.0d does not change project format 15, Hot/Warm/Cold schema 16, colour-state schema 4, adjustment schema 10 or vector schema 7. It does not alter document output defaults, display management, soft proofing, ICC/OCIO interpretation, GPU parity gates, Undo/history or the 0.11.0i.3 shutdown lifecycle.
