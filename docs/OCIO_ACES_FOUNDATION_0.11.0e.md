# 0.11.0e — OpenColorIO and ACES Foundation

## Scope

This stage associates a validated OCIO configuration with a document and makes eligible OCIO working spaces available to Assign Profile and Convert to Profile. It does not yet apply the saved display/view transform to the canvas.

## Configuration choices

- Bundled/versioned ACES 2 CG config URI
- Bundled/versioned ACES 2 Studio config URI
- The configuration referenced by the `OCIO` environment variable
- An external `.ocio` or `.ocioz` configuration
- Disabled, for ICC-only documents

The saved dependency includes the configuration source, resolved identifier, version, fingerprint and bridge space. On reopen, a missing config remains missing and a changed fingerprint requires explicit relinking. VFX Photo Lab never substitutes another config based on a similar display name.

## Working-space conversion

OCIO-to-OCIO conversion uses a processor from the active config. ICC-to-OCIO and OCIO-to-ICC conversions use OpenColorIO's built-in `sRGB - Texture` interchange and the cached Qt ICC transform where necessary. Conversion includes raster pixels and semantic vector/text/Gradient Map colours, preserves Alpha, and transforms hidden RGB beneath zero Alpha.

The 0.11.0e working-space picker intentionally exposes only OCIO spaces that can also be represented by an exact Qt matrix/TRC proxy: ACEScg, ACES2065-1 when supported by the active Qt build, Linear Rec.709 and sRGB Texture. This keeps the 0.11.0d encoded-versus-linear CPU adjustment contracts correct after conversion. Camera-log and other complex encodings remain visible in configuration information but are not offered as authoritative integer document working spaces yet.

## ACES limitation

RGBA8 and RGBA16 document storage cannot preserve negative or above-one scene-linear values. ACEScg and ACES2065-1 are therefore supported only within the current 0–1 document range. Full unbounded ACES authoring requires later floating-point document storage.

## Display/view foundation

A configuration's display, view and optional look selection may be validated and stored. This is dependency/state groundwork for 0.11.0f; it does not bake a view transform into pixels or change the current canvas.
