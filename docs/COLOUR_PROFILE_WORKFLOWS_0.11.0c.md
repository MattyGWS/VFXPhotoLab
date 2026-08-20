# 0.11.0c — Working spaces, Assign Profile and Convert to Profile

## Scope

0.11.0c turns the colour state introduced in 0.11.0a and the ICC ingress metadata introduced in 0.11.0b into explicit document workflows. It does not yet change general adjustment mathematics, add display management, or add export profile conversion.

## Working-space choices

New documents can be created in sRGB, Linear sRGB, Display P3, Adobe RGB or ProPhoto RGB. Existing documents can use those built-in spaces or a supported `.icc`/`.icm` file through **Image → Colour Management**.

## Assign Profile

Assign Profile changes the interpretation metadata attached to the current document values. It does not alter RGB component values. The canvas source and every colour-bearing raster layer are retagged transactionally. Input-origin metadata is retained, and an originally untagged source records the newly assigned interpretation without pretending that the source file contained an embedded profile.

Assign is useful when an image is untagged or incorrectly tagged and the user knows which profile describes its existing values. Because values are not converted, changing the assigned profile can change appearance.

## Convert to Profile

Convert to Profile obtains one cached Qt ICC colour transform from the central transform service and reuses it to rewrite document RGB values into the selected working profile while preserving colourimetry. The operation covers:

- The authoritative document canvas.
- Every raster layer, including RGB hidden beneath zero alpha.
- Vector fill and stroke colours.
- Text-layer colours.
- Gradient Map stop colours.

Alpha, masks, selections and guide data are not colour converted. The worker prepares a complete replacement state before committing anything, supports cancellation between payloads, rejects stale results if the document changes, and records one structural Undo command.

## Persistence and residency

The existing version-15 `.vfxphoto` colour state already stores built-in and external ICC descriptors, including embedded profile bytes and fingerprints. Public project load and private Hot/Warm/Cold restore now explicitly retag all raster payloads with the restored working profile so layer metadata cannot drift from the document state.

## Compatibility boundary

Explicit Assign or Convert switches the document to Managed V1 colour processing. Existing projects remain Legacy V1 until the user performs an explicit profile operation. General adjustment-domain correctness arrives in 0.11.0d; display transforms arrive in 0.11.0f. Therefore this release establishes correct document values and metadata, but complex adjusted documents may not yet preview identically across all working spaces until those later stages are complete.
