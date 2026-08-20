# 0.13.0g.2 — Global Snapping and Pixel-Aligned Geometry

This fix-up consolidates the existing tool-specific snapping switches behind one
application preference without changing project or preset schemas.

## Global control

The status area now contains an always-visible, checkable **Snap** button beside
the zoom controls. **View → Enable Snapping** and `Ctrl+Shift+;` address the same
state. The preference is stored as `tools/snappingEnabled`; the previous
Transform and Crop settings are kept coherent for downgrade compatibility.

The master state enables each operation's own snapping policy:

- Guides use document pixel edges and centres.
- Vector creation and anchor editing use document pixel boundaries.
- Transform retains its existing document, guide, layer and point targets.
- Crop retains its existing canvas, guide, selection and visible-layer targets.

The Transform toolbar retains **Snap Distance**, because its screen-pixel
threshold remains specific to transform/path target acquisition. Duplicate
Transform and Crop on/off checkboxes were removed.

## Guide lattice

While snapping is enabled, new and moved guides are quantised to half-pixel
coordinates. Integer coordinates are pixel edges; half-integer coordinates are
pixel centres. A 127 px axis therefore has an exact centre at 63.5.

Existing saved guide coordinates are not changed on load. They adopt the new
lattice only when dragged while snapping is enabled.

## Vector lattice

While snapping is enabled:

- Rectangle, rounded rectangle, ellipse, polygon, star, arrow and line creation
  previews and committed bounds use integer document coordinates.
- New Pen anchors use integer document coordinates.
- Direct Selection anchor drags and keyboard nudges use integer document coordinates.
- Existing target snapping remains available, but half-pixel centre guides are
  not offered as anchor targets because vector anchors must remain on pixel
  boundaries.

Bezier control handles and Corner-tool radius handles keep unrestricted
subpixel precision. Existing vector geometry is not rounded when a project is
opened. Disabling Snap restores free subpixel placement.

This change aligns filled vector boundaries with the pixel grid. Centred stroke
crispness still depends on stroke width and alignment and is intentionally not
silently offset here.
