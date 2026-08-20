# 0.13.0g.4 — Dedicated Tool Icon Assets

## Scope

This fix-up changes only toolbar artwork ownership and resource wiring. Tool activation, family menus, shortcuts, rendering, vector geometry, snapping and persistence remain unchanged.

## Dedicated resources

Every selectable toolbar action now references a unique PNG filename. The seven vector shape subtypes have separate `shape-*.png` assets, and Pen, Direct Selection and Corner have separate path-tool assets. Line and Arrow no longer refer to the absent `transform-skew.png` file, so both are visible in the Shape family menu and when selected.

Transform and Clone Stamp use explicit `transform.png` and `clone-stamp.png` names. Selection resources remain reserved for selection tools and are no longer reused by vector tools.

## Replacement contract

All toolbar assets are transparent 24 × 24 RGBA PNGs. The application uses their Alpha channel as a mask and recolours them for the current theme. Replacing one file therefore changes only its mapped tool and does not require a code change or separate light/dark artwork. The complete stable mapping is listed in `resources/icons/README.md`.

`scripts/check-tool-icons.py` validates dimensions, RGBA format, Qt resource inclusion and unique tool ownership before each Linux or Windows build. During toolbar construction, the application also checks that each claimed icon can be loaded and that no second tool claims the same path. A regression produces a focused startup warning instead of silently leaving a button blank or sharing artwork.

## Compatibility

No document or application behaviour is serialized by these assets. Project, vector, adjustment, preset, Hot/Warm/Cold, Undo, export, queue and recovery schemas are unchanged.
