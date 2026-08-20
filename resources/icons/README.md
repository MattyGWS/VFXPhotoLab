# Tool icon assets

Every selectable toolbar tool and every vector shape subtype owns a separate PNG resource so an icon can be replaced without changing another tool.

## Asset contract

- Canvas: **24 × 24 pixels**
- Format: **RGBA PNG** with a transparent background
- Artwork: the application uses the source Alpha as a mask and recolours it for the active theme
- Keep the filenames stable; they are compiled through `resources/resources.qrc`

## Tool mapping

| Tool | File |
|---|---|
| Transform | `transform.png` |
| Rectangle Select | `select-rectangle.png` |
| Ellipse Select | `select-ellipse.png` |
| Freehand Lasso | `select-freehand-lasso.png` |
| Polygonal Lasso | `select-polygonal-lasso.png` |
| Brush | `brush.png` |
| Eraser | `eraser.png` |
| Crop | `crop.png` |
| Clone Stamp | `clone-stamp.png` |
| Healing Brush | `healing-brush.png` |
| Spot Healing | `spot-healing.png` |
| Patch Tool | `patch.png` |
| Dodge | `dodge.png` |
| Burn | `burn.png` |
| Sponge | `sponge.png` |
| Blur | `blur.png` |
| Sharpen | `sharpen.png` |
| Smudge | `smudge.png` |
| Fill | `fill.png` |
| Gradient | `gradient.png` |
| Rectangle Shape | `shape-rectangle.png` |
| Rounded Rectangle Shape | `shape-rounded-rectangle.png` |
| Ellipse Shape | `shape-ellipse.png` |
| Line Shape | `shape-line.png` |
| Polygon Shape | `shape-polygon.png` |
| Star Shape | `shape-star.png` |
| Arrow Shape | `shape-arrow.png` |
| Pen Tool | `pen.png` |
| Direct Selection Tool | `direct-selection.png` |
| Corner Tool | `corner-tool.png` |
| Eyedropper | `eyedropper.png` |
| Text | `text.png` |

`image-layer.png` is a layer-type icon rather than a toolbar tool and follows the same 24 × 24 RGBA convention.
