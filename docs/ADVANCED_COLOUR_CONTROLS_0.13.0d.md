# 0.13.0d — Advanced Colour Controls

0.13.0d completes the planned advanced-colour group without changing the public project container, private Hot/Warm/Cold envelope, colour-management state, vector payload, export profile, production plan, queue or recovery schema.

## Retained controls

Colour Balance, Channel Mixer and Black & White were already established adjustment types. Their serialized parameters and rendering equations are retained. This stage expands useful built-in presets and hardens Inspector interaction:

- Colour Balance range switching now updates the visible sliders without emitting synthetic document changes.
- Channel Mixer output/monochrome switching is likewise signal-blocked.
- Channel Mixer shows the selected Red + Green + Blue coefficient total. The readout is UI-only and is not persisted.
- Black & White gains practical red-filter and blue-filter presets alongside the existing high-contrast and tint workflows.

## Selective Colour

Selective Colour is append-only adjustment identifier 22. It provides nine fixed ranges:

- Reds, Yellows, Greens, Cyans, Blues and Magentas
- Whites, Neutrals and Blacks

Each range has Cyan, Magenta, Yellow and Black controls from -100% to +100%. The method is either Relative or Absolute.

Absolute applies direct percentage-point process-colour corrections. Relative scales the correction by the process-colour amount already present. A pure specular white pixel therefore remains unchanged by a Relative colour correction, while Absolute may change it.

Range influence is calculated from the original encoded RGB triplet. Primary and secondary families use maximum/middle/minimum component distances, preserving overlap at tied extrema. Whites and Blacks use bounded component distance above or below 50%; Neutrals use the remaining midtone span. All corrections for one pixel are accumulated before clamping.

## Colour and Alpha contract

Selective Colour runs in the explicit encoded-sRGB adjustment domain used by the existing family-based controls. ICC, OpenColorIO and ACES working spaces enter and leave that domain through the established managed adjustment lattices. Monitor, proof and output transforms never become layer pixels.

Alpha is copied exactly. Hidden RGB beneath zero Alpha is processed by the same independent straight-RGB reference used by other adjustment layers. The operator has matching RGBA8 and RGBA64 CPU paths and an independently implemented WGSL path.

## GPU and cache integration

The GPU adjustment uniform appends nine aligned range vectors plus one method/options vector. Its compile-time checked size advances from 640 to 800 bytes. ID 22 receives a feature approval only after direct CPU/WGSL parity succeeds, and managed working spaces also require the managed-domain parity case. Failure selects the exact CPU reference for the affected stack only.

Tile cache fingerprints include all 36 range values and the method. Selective Colour is point-local, so it adds no halo or neighbouring-tile dependency.

## Compatibility

Adjustment JSON advances from schema 12 to schema 13. Schema 13 is the first schema allowed to contain Selective Colour. Older schemas preserve their original meanings and a payload pairing ID 22 with schema 12 or earlier is rejected.

Unchanged envelopes:

- `.vfxphoto` project format 15
- Hot/Warm/Cold snapshot schema 16
- colour-state schema 4
- vector schema 7
- unified preset envelope
- export profiles and production plans
- queue descriptions and recovery records

Generic adjustment-layer handling carries the new type through masks, groups, Pass Through, Undo, project persistence, residency, Quick Export, Production Export and immutable queue snapshots.

## Deterministic coverage

The stage adds tests for append-only identities, schema-13 round trips, dishonest legacy-schema rejection, exact identity at both bit depths, 8/16-bit consistency, chromatic and tonal range direction, Relative/Absolute behaviour, specular-white behaviour, Alpha and hidden RGB, managed Display-P3 processing, full-frame/region equivalence, built-in presets, WGSL publication and Cold restoration.

## Reference contracts

The Relative/Absolute user-facing semantics follow Adobe's published Selective Color description. The independently written range-scale and component-correction implementation was cross-checked against FFmpeg's LGPL selective-color reference:

- https://helpx.adobe.com/photoshop/using/mix-colors.html
- https://github.com/FFmpeg/FFmpeg/blob/master/libavfilter/vf_selectivecolor.c
