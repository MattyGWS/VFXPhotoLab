# 0.13.0g.9 — Crop Rotation Undo and Angle Scrubbing

## Failure traced

Crop Apply captured one complete `DocumentState` before starting its asynchronous preparation. That state correctly included the submitted Crop frame and Straighten angle so the finished worker result could be rejected if the user changed the document or Crop options while it was running.

The same state was also stored as the Undo side of the structural Crop command. For a rotated crop, Undo therefore restored the original document pixels and layer tree **and** restored the non-zero pending Crop angle. `syncCropCanvas()` then presented the original document through that live angle again. The structural Undo had happened, but the active Crop preview made the canvas appear unchanged.

Pending Crop field changes are intentionally not independent history commands. Once the Crop command had been undone, no earlier command could reset that restored angle to zero. Earlier paint or layer history could still move underneath the preview, but remained visually obscured by the persistent rotated Crop presentation.

## Corrected history contract

Crop Apply now retains two snapshots:

- **Submitted state:** the exact frame, angle, options, layers, pixels, selection, guides and edit target used to validate the asynchronous result before commit.
- **History before-state:** the same original document data with the Crop tool settled to the full original canvas, original ratio, inactive Straighten sampling and a `0°` angle.

The committed after-state uses the same settled Crop-tool contract for the new canvas. Undo and Redo therefore move only between durable document results; they do not resurrect the temporary Crop gesture that produced those results.

Persistent Crop preferences remain unchanged: Crop mode, overlay, overlay orientation, dim opacity, snapping and Delete Cropped Pixels are retained. No crop resampling, transform construction, layer/mask processing, Alpha-safe hidden RGB, 8/16-bit precision, GPU/CPU routing or project serialization changed.

## Transform angle consistency

Transform Angle now uses the existing compact combined scrubber/value field rather than a plain spin box. It supports horizontal scrubbing from `-180°` to `180°`, 0.1° increments, Shift fine adjustment, Ctrl coarse adjustment and exact typed entry. The pending Transform session and one-command Apply/Undo model are unchanged.

## Regression coverage

A focused core test verifies that settling a submitted Crop state:

- restores a full-canvas frame and fixed size;
- restores the original canvas ratio;
- disables Straighten sampling;
- resets Angle to `0°`; and
- preserves persistent Crop preferences.
