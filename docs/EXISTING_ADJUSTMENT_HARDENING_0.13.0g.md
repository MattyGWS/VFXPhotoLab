# 0.13.0g — Existing Adjustment Improvements and Hardening

## Scope

This final 0.13.0 stage improves existing adjustment workflows and validates integration rather than adding another persisted adjustment identifier. Adjustment JSON remains schema 15. Project, residency, colour, vector, preset, export, queue and recovery formats remain unchanged.

## Curves

- **Sample Point** temporarily activates the existing image eyedropper and adds a point to the currently selected RGB, Red, Green or Blue curve.
- RGB sampling uses the same encoded Rec.709 luminance convention as the Curves histogram; component channels use the exact sampled component.
- A nearby existing point is selected instead of creating an almost-duplicate point.
- The inserted point is initialised from the current curve evaluation, then participates in the normal grouped Undo workflow.
- **Reset All** restores all four channels, interpolation and histogram-display defaults in one history step.

## Hue/Saturation ranges

- The range strip displays the exact core-width and smooth feather shape used by the processor, including hue-wheel wrapping.
- **Sample Centre** sets the current range centre from a coloured image sample. Near-neutral samples are rejected rather than assigning an undefined hue.
- **Reset Range** restores only the selected range's default centre, width, feather and corrections.
- The selected range survives Inspector rebuilds, including the rebuild used after an on-image sample.

## Gradient Map

- Stops can be duplicated without changing their colour.
- Interior stops can be distributed evenly while preserving order and colour.
- Delete/Backspace removes an interior stop; arrows nudge; Shift+arrows performs a fine 16-bit-scale nudge; Ctrl+arrows selects adjacent stops; Enter edits the selected colour.
- All actions retain the existing two-stop minimum, 64-stop maximum and minimum stop-gap validation.

## Histogram performance and determinism

- Images below one megapixel retain the low-overhead serial path.
- Larger images use a private pool capped at eight workers; exact 16-bit histograms cap at six workers to bound local-bin memory.
- Each worker owns independent luminance/R/G/B/Alpha bins and counters. Reduction is ordered and byte-for-byte deterministic.
- Selection coverage, transparent-pixel exclusion from RGB/luminance, managed sRGB luminance conversion, cancellation, cache identity and stale-request rejection retain their previous meaning.

## Isolation and safety

One-shot Levels, Curves, Hue/Saturation and White Balance sampling requests are mutually exclusive. They are cleared when leaving the Eyedropper tool, rebuilding the Inspector or switching document sessions, preventing a sample intended for one document or adjustment from mutating another.
## GPU/CPU and integration review

The native chained-adjustment parity case now includes nontrivial Curves and Gradient Map operators alongside the existing Levels, Hue/Saturation, Vibrance, White Balance and Colour Balance coverage. The established feature-specific approval gate remains authoritative; no CPU-only adjustment is silently admitted to the native path.

No persisted structures changed in this stage. Existing generic project, Hot/Warm/Cold, preset, Quick Export, Production Export, queue snapshot and recovery paths continue to carry the same schema-15 adjustment data.
