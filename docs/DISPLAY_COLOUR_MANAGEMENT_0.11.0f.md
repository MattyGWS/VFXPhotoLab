# 0.11.0f — Display Colour Management and Soft-Proof Foundation

## Scope

0.11.0f manages only presentation surfaces such as the canvas and document-strip thumbnail copies. It does not rewrite the document working image, cached working thumbnails or any editable object.

```text
working-space preview
  └─ optional proof ICC simulation
       └─ monitor ICC transform OR OCIO Display/View
            └─ QPainter presentation copy
```

## Monitor profile selection

Priority is:

1. Manual profile selected in **Image → Colour Management → Display Colour Management and Soft Proofing**.
2. `VFXPHOTOLAB_MONITOR_ICC` environment override.
3. Windows Color System on Windows, or colord/`colormgr` on Linux.
4. sRGB fallback.

The UI identifies the active screen, profile, discovery source and fallback status. The result is cached by screen identity and override path, then refreshed when the window changes screens or the screen list changes.

## ICC and OCIO renderer separation

ICC working/display/proof conversions use the existing `ColourTransformService`. Any conversion involving an OCIO colour space uses the ordinary `OcioCpuTransform`. Final OCIO presentation uses a separate `OcioDisplayTransform` that explicitly creates `OCIO::DisplayViewTransform`; optional looks are validated and processed independently. A missing or fingerprint-mismatched config causes unmanaged presentation fallback rather than substitution.

## Soft proofing

Soft proofing stores an embedded ICC descriptor, enable state, rendering-intent request, black-point-compensation request and gamut-warning state. The initial gamut warning is a deterministic source→proof→source round-trip comparison and highlights sufficiently different RGB values in magenta. It is a foundation, not a replacement for a full LittleCMS proofing pipeline.

## Presentation cache isolation

`ImageCanvas` stores working-space and display-space copies separately for:

- the main preview backing image;
- authoritative and coarse progressive tiles;
- live stroke previews;
- transform backgrounds, foregrounds and composite previews;
- document-strip thumbnail copies derived from raw residency thumbnails.

A display-transform fingerprint change rebuilds only display copies. The working copies remain authoritative for edits, colour sampling, history, copy/merge and export.

## Compatibility and persistence

Colour-state schema 4 introduces an explicit presentation gate. New managed documents enable automatic monitor ICC presentation. Schema-1/2/3 states load with the gate disabled, retaining the appearance they had before 0.11.0f until the user opts in. Display and proof settings are saved in `.vfxphoto` projects and private residency snapshots and remain isolated per document.

Display/proof changes mark the document as needing save but do not create an Undo item. Undoing an unrelated edit retains the live display/proof selection wherever it remains safe with the restored OCIO configuration.

## Deferred to 0.11.0g and later

- GPU/WGSL ICC/OCIO transform resources and shaders;
- asynchronous display-transform scheduling;
- floating-point/HDR document storage;
- full proof-engine intent/BPC control beyond the capabilities exposed by Qt;
- colour-managed export, output bit-depth conversion and dithering.
