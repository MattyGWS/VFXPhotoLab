# 0.13.0g.14 — Expanded Vignette Geometry and On-Canvas Controls

## Problem

The original Vignette used a nominal ellipse whose horizontal and vertical radii were exactly half the current document dimensions. Midpoint and Feather could only move the transition inward from that geometry. As a result, a conventional dark vignette always affected the centres of the image edges before it reached the corners; users could not enlarge the geometry beyond the canvas for a corners-only treatment.

## Persisted geometry

`VignetteParameters::size` is a uniform percentage in the range 10–400. The renderer multiplies the document half-width, half-height and minimum half-span by `size / 100` before applying the established Roundness blend, superellipse exponent, Midpoint and Feather equations.

Size 100 is deliberately the old equation, preserving existing rendering exactly. Adjustment JSON schema 16 stores the new field. Schema-14 and schema-15 Vignette payloads load with Size 100. The public project, Hot/Warm/Cold, preset envelope, production plan, queue and recovery formats already embed versioned adjustment data and require no envelope change.

## Canvas presentation and editing

When one Vignette adjustment is selected and Show on-canvas controls is enabled, `ImageCanvas` draws:

- the orange Midpoint/start boundary;
- the cyan nominal Size boundary;
- the white Feather/end boundary;
- a centre cross;
- a yellow Size handle that may lie outside the document;
- an orange Midpoint handle; and
- a cyan Rotation handle.

The paths use the same inverse-radius Roundness blend and superellipse exponent as the CPU renderer. They are painted in viewport space after image/crop presentation, are not clipped to document bounds and never enter exported pixels.

A drag begins one ordinary property-Undo transaction. Pointer samples update the selected adjustment and use the existing coalesced adjustment preview. Release records one history command and permits the normal settled render. Escape restores the initial values; because the before and after document states match, no effective Undo entry remains.

## Preserved contracts

- exact straight hidden RGB and source Alpha;
- RGBA8 and RGBA64 CPU references;
- masks, isolated groups and Pass Through groups;
- region/full-frame equivalence and tile-cache identity;
- project and preset compatibility through explicit schema migration;
- ICC, OpenColorIO, ACES, display management and soft proofing;
- Hot/Warm/Cold residency, export queues and recovery;
- no Smart Layer or live-filter work before 0.14.0.
