# 0.11.0h — Colour-Managed Export, Bit Depth and Blue-Noise Dithering

## Boundary

0.11.0h introduces a dedicated export pipeline. It consumes the authoritative full-resolution hidden-RGB composite produced in the document working space. It does not consume any canvas presentation copy.

```text
working-space composite
→ ordinary working-to-output colour-space transform
→ optional output-space matte compositing
→ explicit integer encoding and optional RGB dither
→ file writer and supported ICC metadata
```

The following are deliberately excluded: active monitor ICC, manual monitor override, OCIO Display/View, proof simulation and gamut-warning overlay. Those remain presentation-only. Export also does not rewrite the document, its layers, semantic colours, masks, selections or history.

## Output colour selection

The exporter can keep the document working RGB values or convert to a selected ICC/OCIO output descriptor. A fingerprint-matched OCIO configuration exposes all non-data colour spaces as export destinations; these are deliberately broader than the safe proxy-backed subset allowed for document working spaces. Ordinary conversion uses `ColourTransformPurpose::WorkingToOutput`; OCIO Display/View uses a different API and cannot enter this path. A missing or fingerprint-mismatched OCIO configuration is an error rather than a substitution.

Output profile, intent, black-point-compensation request and ICC-embedding preference are stored in `DocumentColourState::output`. Updating those defaults leaves the colour processing revision unchanged and creates no Undo operation. Project and private residency snapshots already serialize the complete colour state.

## Precision order

The flattened composite is promoted to straight `QImage::Format_RGBA64` before colour conversion. This keeps 16-bit source precision available to ICC/OCIO transforms and provides a high-precision source for final 8-bit reduction. An 8-bit document cannot gain source precision, but it follows the same deterministic ordering.

PNG and TIFF can request 16-bit integer output. The current integer document architecture represents export transform values in the 0–1 channel range; scene-linear OCIO values outside that range are clipped by the integer surface and this limitation is reported in the advanced documentation rather than silently treated as floating-point export. Other supported flattened-image formats are constrained to 8-bit in this stage. Writer capability is validated before background rendering begins.

## Alpha and matte handling

Alpha-capable outputs retain straight Alpha and hidden RGB. Alpha is not colour transformed.

For JPEG/BMP and other declared no-Alpha outputs, transparency is composited against a user-selected matte after working-to-output conversion. The UI matte is interpreted as sRGB and converted to the selected output colour space first. The result is then made opaque and quantised.

## Blue-noise reduction

The reducer uses a checked-in 64 × 64 permutation of 4096 blue-noise ranks. The tile phase is deterministic from the fixed export seed and image dimensions.

- The same threshold is used for R, G and B, preventing neutral grey ramps from acquiring coloured noise.
- Values exactly at 0 and 65535 remain exact 0 and 255.
- Alpha uses deterministic nearest quantisation and is never dithered.
- The transform is deterministic across runs for identical dimensions, pixels and settings.

## Metadata rules

PNG, JPEG and TIFF can attach an ICC `QColorSpace` in the current writer path. OCIO colour spaces contain no ICC payload and are saved untagged. TGA, BMP and WebP are conservatively treated as untagged in this stage. The user’s document-level embedding preference is preserved even when a particular format cannot honour it.

## Cancellation and atomicity

Full-resolution rendering, colour conversion, matte compositing and quantisation observe the existing export cancellation token. Qt-backed writers use `QSaveFile`; TGA retains its existing atomic `QSaveFile` codec. A cancelled operation does not publish a partially prepared image.

## Completed by 0.11.0i

The final milestone stage broadens persistence/residency regression coverage, audits malformed and unavailable colour resources without substitution, hardens deterministic error handling and completes the colour-management conformance/documentation pass. Batch queues and production export presets remain 0.12.0 work.
