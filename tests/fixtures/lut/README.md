# Deterministic LUT conformance fixtures

These files are intentionally tiny and hand-auditable. They use the Resolve/Cube
red-fastest 3D row convention: red changes first, then green, then blue.

`vectors.json` is the authoritative set of scalar inputs and expected outputs
for the independent test evaluator. Cases tagged `matches-current` document behaviour that the authoritative
scalar evaluator now matches, including extended output values that are no
longer clamped by `CubeLut::evaluate()`.

The valid fixtures cover identity, deliberately asymmetric red-fastest lattice
ordering, red/blue swapping, inversion, channel isolation, range clipping and
remapping, stepped tables, non-default per-channel domains, specific 1D/3D input
ranges, a combined 1D shaper plus 3D table, extended output values, finite values
outside the removed ±16 guard, wide finite domains and fractional values that exposed the former 8-bit GPU LUT texture quantisation and now verify direct RGBA16Float packing.
`interpolation_probe_3d.cube` is deliberately non-linear so trilinear and
tetrahedral interior samples differ, including exact fraction-tie cases.

`invalid/` contains deterministic parser-rejection fixtures for duplicate title,
size, domain and range declarations, ambiguous generic/specific ranges,
recognised but unsupported input/output video-range flags, unknown directives,
directives after table data, missing/extra rows and a range declared for a
missing table.

0.10.1e reuses the identity 3D fixture to isolate and verify the surrounding
Tony McMapface and AgX Base sRGB operator mathematics independently from the
creative contents of their production tables. Separate tests cover filename and
`TITLE` suggestion, schema-10 persistence/migration, project save/reopen,
Strength space, the 0.10.1e authoritative CPU path and RGBA8/RGBA64 alpha-safe rendering.

0.10.1f additionally uses these fixtures to verify direct binary16 table packing,
trilinear/tetrahedral lookup independence, transfer-aware GPU eligibility and
selective CPU fallback for values or domains outside the native transport
contract.
