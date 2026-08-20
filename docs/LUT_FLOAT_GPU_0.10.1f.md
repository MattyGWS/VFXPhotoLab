# 0.10.1f floating-point GPU LUT contract

## Lookup representation

A validated LUT is packed into one RGBA16Float 2D texture. A cube of edge `N` occupies `N² × N` texels with `x = red + blue × N` and `y = green`. A 1D shaper, when present, occupies the row immediately below the cube and uses one texel per table entry. Alpha is set to one and ignored.

This representation preserves finite negative and greater-than-one table samples within binary16 range and avoids the former RGBA8 lookup quantisation. Domains remain f32 uniforms because shader coordinates are evaluated in f32.

## Evaluation parity

CPU and WGSL use the same order:

1. interpret the document transfer;
2. apply a named operator's input transform when selected;
3. clip/sample the optional per-channel 1D shaper;
4. clip/sample the red-fastest 3D table;
5. use persisted trilinear or deterministic tetrahedral interpolation;
6. apply a named operator's output transform when selected;
7. return to document component space;
8. blend Strength against the original input.

The native adjustment tile remains RGBA8. Floating-point table values and intermediate arithmetic therefore survive through the LUT operation but are clamped/quantised at that tile's destination write. The 16-bit image path remains the RGBA64 CPU reference.

## Feature approval and fallback

Startup parity independently exercises tetrahedral, trilinear, encoded/linear conversion, Tony McMapface and AgX Base sRGB. LUT acceleration is published only if all cases pass. A table outside binary16 range, a domain outside finite ordered f32, an oversized lookup or native allocation/pipeline failure routes LUT-containing rendering through the CPU reference without disabling unrelated GPU features.

## Source-of-truth rule

`shaders/adjustment_tile.wgsl` is the only adjustment WGSL source. CMake embeds it into a generated header at configure time; the build-time validator reads the same file.
