# 0.13.0g.5 — Aligned Colour Pair Controls

## Scope

This fix-up only changes the compact primary/secondary colour control at the top of the Colour dock. No colour values, brush/vector semantics, persistence or document formats change.

## Geometry

- Pair widget: 50 × 50 px.
- Primary swatch: 30 × 30 px at `(2, 2)`.
- Secondary swatch: 30 × 30 px at `(18, 18)`.
- Horizontal and vertical offset: exactly 16 px.
- Reset hit target: complete 18 × 18 top-right wedge.
- Swap hit target: complete 18 × 18 bottom-left wedge.

The action hit areas meet the swatch edges without overlapping them, so clicks on a colour square continue to activate that swatch while clicks anywhere in either exposed corner invoke the corresponding action.

## Appearance and behaviour

Both action buttons have transparent backgrounds and borders in every state. Only their 15 × 15 icons are visible. Swap uses a drawn two-arrow icon instead of a font glyph, avoiding platform/font alignment differences; Reset retains the established black/white-square icon. Both icons are regenerated after theme changes.

Tooltips, accessible names, primary/secondary activation, double-click colour dialogs, saved preferences and colour consumers remain unchanged.
