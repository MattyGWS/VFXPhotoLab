# 0.11.0d — Colour-correct CPU processing contracts

## Purpose

0.11.0d makes adjustment mathematics explicit for managed documents without changing any Legacy V1 project. It does not add display management or a new WGSL colour-transform implementation.

## Domains

| Domain | Initial adjustments | Contract |
|---|---|---|
| Linear working | Exposure | Convert to the document primaries with a linear transfer function, evaluate, then return to the encoded working space. |
| Encoded working | Levels, Curves, Posterise | Operate directly on encoded values in the current working space. |
| Encoded sRGB / Rec.709 | Contrast, Saturation, Hue/Saturation, Vibrance, White Balance, Colour Balance, Black and White, Gradient Map, luminance Threshold, Shadows/Highlights | Convert from the working profile to sRGB, run the established Rec.709/HSL/Oklab operator, then convert back. |
| Raw components | Channel Mixer and channel-based Threshold | Never infer primaries or a transfer function. |
| LUT contract | LUT | Preserve the specialised 0.10.1 encoded/linear/raw/Tony/AgX rules unchanged. |

Layer blend modes remain explicit encoded-working-space operations. They do not contain an implicit sRGB transfer or primary conversion, so no additional domain adapter is required in this stage.

## Compatibility

Legacy V1 bypasses all domain transforms. Existing projects therefore retain the same component values and adjustment output. Managed V1 performs domain transforms through `ColourTransformService`; Alpha is copied unchanged and straight hidden RGB remains colour-bearing data.

## GPU boundary

0.11.0d is the CPU-correctness stage. At that stage, managed stacks containing Linear Working or Encoded sRGB/Rec.709 adjustments used the CPU reference compositor, while unaffected stacks could remain GPU accelerated. **0.11.0g now fulfils the deferred GPU boundary** with parity-gated working↔domain lattices for the 8-bit tiled compositor; the exact CPU path remains the fallback and the 16-bit compositor remains CPU-authoritative.
