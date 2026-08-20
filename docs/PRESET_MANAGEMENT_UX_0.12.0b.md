# 0.12.0b — Preset Management UX

## Scope

This stage exposes the 0.12.0a unified preset architecture through one reusable management dialog for adjustment presets and vector appearance presets. It does not introduce export profiles, naming templates, multi-output export or queue processing; those remain later 0.12.0 stages.

## Interaction model

Ordinary use remains compact:

- Adjustment presets remain directly available from the selected adjustment layer's Inspector combo.
- Vector appearance presets remain available from the existing Appearance menu used by Shape and Pen workflows.
- Advanced organisation opens only through **Manage…** or **Appearance Presets…**.

The shared manager provides live search over name, category and tags; source, category, favourites and recently-used filters; separate built-in and user groups; and a details pane containing identity, usage and feature-specific settings summaries.

## Operations

User presets support:

- create from the current adjustment layer or current vector appearance;
- update stored settings from the current source;
- rename while preserving the stable ID;
- duplicate with a fresh stable ID;
- edit category and tags;
- favourite/unfavourite;
- delete with confirmation;
- validated import and export.

Built-in adjustment presets remain immutable. They may be applied, favourited, exported or duplicated to a new user preset. Built-in favourite/recent state is stored separately from the built-in definition.

## Usage metadata

`PresetUsageStore` writes a bounded `usage.vfxpreset.json` sidecar beneath the preset app-data root. Entries are keyed by the canonical stable preset ID and contain only favourite, last-used and use-count values. Writes use `QSaveFile`; malformed entries are ignored independently.

The sidecar is primarily required for immutable built-ins. User preset metadata continues to live in the preset envelope itself. Neither path enters `.vfxphoto` projects, private Hot/Warm/Cold snapshots, Undo history, document modified state, processing revisions or renderer cache keys.

## Adjustment integration

The manager is bound to the adjustment layer that opened it. Apply performs one structural Undo operation on that layer only. Save/update reads the layer's current effective adjustment data at the moment the operation is invoked, so leaving the manager open cannot cause a stale parameter snapshot to be written.

LUT presets continue to use the existing embedded `AdjustmentData` payload. The details pane reports LUT title/source, named operator profile, processing mode, interpolation and strength without changing generic LUT, Tony McMapface or AgX interpretation.

## Vector integration

Apply targets selected vector layers as the existing appearance operation and records one Undo operation for the selection. With no selected vector layer, Shape or Pen tool defaults are updated. Save/update uses the current selected vector appearance or active Shape/Pen defaults.

The details pane reports fill state and colour plus stroke opacity, width, pattern, cap, join, alignment, miter, dash settings and arrowheads.

## Validation and failure isolation

The manager delegates file and payload validation to the feature stores introduced in 0.12.0a. Wrong-kind, unsupported-version, malformed, oversized or unsafe presets fail without changing existing presets. A failed import, export or mutation leaves the current selection and completed files intact. Names, categories and tags retain the shared envelope bounds.

## Compatibility boundary

Project format remains 15, private residency schema remains 16, colour-state schema remains 4, adjustment schema remains 10 and vector schema remains 7. No export request, render, colour-transform, GPU/CPU compositor, Smudge or shutdown-lifecycle code is changed in this stage.
