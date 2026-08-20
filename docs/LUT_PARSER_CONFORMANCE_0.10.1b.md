# LUT parser and data-model conformance — 0.10.1b

## Scope

This stage corrects the production `.cube` parser and persisted LUT payload. It
does not yet replace the trilinear evaluator, remove its final `[0,1]` clamp,
define encoded-versus-linear processing or replace the RGBA8 GPU lookup texture.
Those remain isolated work for 0.10.1c, 0.10.1d and 0.10.1f.

## Parser contract

The parser now has four explicit sections: header, 1D data, 3D data and complete.
All declarations must appear before the first table row. A combined file consumes
exactly `LUT_1D_SIZE` rows first and then exactly `LUT_3D_SIZE³` rows. Extra rows,
missing rows and text/directives after data are rejected.

Supported declarations are:

- `TITLE`
- `LUT_1D_SIZE`
- `LUT_3D_SIZE`
- `DOMAIN_MIN` / `DOMAIN_MAX`
- `LUT_1D_INPUT_RANGE`
- `LUT_3D_INPUT_RANGE`

Every size/range declaration is unique. Generic `DOMAIN_*` and LUT-specific input
ranges are not assigned an arbitrary precedence: mixing them is rejected as
ambiguous. A LUT-specific range without its corresponding table is also rejected.
Unknown directives fail with their name and line number.

`LUT_IN_VIDEO_RANGE` and `LUT_OUT_VIDEO_RANGE` are recognised semantic flags but
are rejected with an explicit explanation because video-range compensation is not
yet implemented. Silently treating such a file as data range would produce a
plausible-looking but incorrect transform.

## Numeric preservation

The old parser and `LutParameters::normalise()` clipped valid table values and
domains to approximately ±16. That guard is removed. Table samples are retained
when finite and representable by the existing float payload. Domain endpoints are
retained as doubles when finite and when their subtraction forms a finite positive
span. Invalid persisted data is rejected/reset rather than rewritten into a
seemingly valid transform.

Extended table samples still mark the LUT as incompatible with the current RGBA8
native lookup texture, so the existing exact CPU fallback remains active. No
extended values are silently compressed into display range.

## Persistence

Adjustment schema 7 adds `shaperDomainSource` and `cubeDomainSource` with these
values:

- `default`
- `domain-directive`
- `input-range-directive`
- `legacy-persisted`

Schemas 1–6 remain loadable. Old non-default domains migrate to
`legacy-persisted`; old default `[0,1]` domains migrate to `default`. The original
header spelling cannot be reconstructed from older saved payloads, but rendering
is unchanged. Public `.vfxphoto` format 14 and private residency schema 15 are not
bumped.

## Deterministic coverage

0.10.1b adds:

- a deliberately asymmetric 2×2×2 fixture proving red-fastest lattice order;
- valid wide-domain and LUT-specific 1D/3D range fixtures;
- invalid fixtures for duplicate titles/sizes/domains/ranges, conflicting range systems,
  input/output video-range flags, unknown directives, post-data directives, absent-table
  ranges and missing/extra rows;
- finite-value and range-source JSON migration/round-trip tests.

The existing independent reference vectors, 8/16-bit checks, project/preset
persistence, hidden-RGB/Alpha coverage, final-clamp baseline and RGBA8
quantisation baseline remain active.
