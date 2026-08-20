# LUT conformance baseline — 0.10.1a

## Purpose

0.10.1a adds a deterministic, independent baseline before the production LUT
parser or evaluator is corrected. A passing baseline does **not** mean the
current implementation is conformant. It distinguishes behaviour that is
already correct from behaviour deliberately pinned as a known defect so later
0.10.1 stages cannot accidentally hide or move the problem.

## Fixture set

`tests/fixtures/lut/` contains small hand-auditable Cube files for:

- 1D and 3D identity;
- red/blue channel swapping;
- inversion;
- red-channel isolation;
- input clipping and range remapping;
- stepped 1D and 3D tables;
- non-default per-channel `DOMAIN_MIN` / `DOMAIN_MAX`;
- a combined 1D shaper followed by a 3D table;
- extended negative and greater-than-one output values;
- finite values outside the current ±16 parser guard;
- fractional values that expose the current 8-bit GPU texture quantisation.

`vectors.json` stores authoritative scalar input/output pairs. The test-only
reference parser and trilinear evaluator are implemented independently in
`tests/test_lut_conformance.cpp`; they do not call `CubeLut` and use the
red-fastest address expression `r + N * (g + N * b)`.

## Baseline results pinned by the suite

The 0.10.0l implementation currently agrees with the authoritative vectors for
basic 1D/3D identity, red-fastest lattice ordering, channel swap, inversion,
channel isolation, generic domain remapping, combined shaper-plus-cube files
and table-sample stepped cases.

The suite also intentionally records these incorrect behaviours:

1. `CubeLut::evaluate()` clamps the strength-blended result to `[0, 1]`, so
   valid extended LUT output is destroyed before a later destination boundary.
2. parsing/normalisation clamps finite table entries to `[-16, 16]`.
3. `CubeLut::buildGpuTexture()` converts decimal samples to `RGBA8888`, proving
   that the current GPU lookup loses table precision before shader sampling.

When the corresponding production fix lands, its baseline test must be moved
from “known defect” to the conformant set rather than deleted.

## Persistence and precision baseline

The conformance executable additionally verifies:

- exact red/blue swap behaviour on RGBA8888 and RGBA64 CPU rendering, including
  hidden RGB under Alpha 0;
- adjustment JSON round trip;
- `.vfxphoto` save/reopen with embedded combined LUT data and identical flattened output;
- user LUT preset save/read/remove with the complete embedded payload.

Project format remains 14, adjustment schema remains 6 and residency schema
remains 15.

## Clipboard document addition

0.10.1a also adds **File → New from Clipboard** and a matching welcome-screen
button. It accepts ordinary platform images and VFX Photo Lab’s private exact
clipboard payload, creates a document at the copied pixel dimensions, retains
8/16-bit integer precision, embedded QColorSpace/ICC data and resolution where
available, assigns sRGB to untagged input, normalises device-pixel ratio to 1,
and creates the copied pixels as the base raster layer. The existing 32,768
pixel document safety ceiling is enforced before a session is opened.
