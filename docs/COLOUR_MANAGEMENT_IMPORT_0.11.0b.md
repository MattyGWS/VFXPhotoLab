# 0.11.0b ICC import and untagged-image policy

## Scope

0.11.0b makes colour-profile ingress explicit. It records and preserves usable embedded ICC information, detects absent or advertised-but-unusable profiles, and applies one consistent untagged-image policy across file open, drag/drop, New from Clipboard and external raster Paste.

File open and New from Clipboard do **not** convert pixel values. Assign Profile and document-wide Convert to Profile are intentionally reserved for 0.11.0c. Paste into an existing differently tagged document retains the established clipboard behaviour: a valid or assumed source profile can be converted into the target document space before insertion.

## Import classification

Every imported image records one of these origins:

- Valid embedded profile
- Valid clipboard profile
- No embedded profile
- Invalid or unsupported embedded profile
- Application-generated content
- Legacy/unknown origin for migrated projects

For supported PNG, JPEG and TIFF containers, a bounded inspector detects profile declarations and retains valid ICC bytes/fingerprints. Qt's decoded `QColorSpace` remains the authoritative usable profile representation. TGA is treated as untagged because the format path has no ICC embedding contract.

## Untagged policy

The application preference offers:

- **Assume sRGB (recommended):** attach an sRGB interpretation without changing component values.
- **Ask every time:** choose Assume sRGB, Leave Untagged or Cancel for each import, with an optional remembered choice.
- **Leave untagged:** keep an invalid `QColorSpace` and preserve raw stored components.

The same resolver is used by normal image open, file-manager drag/drop, New from Clipboard and external image Paste. Internal clipboard payloads retain a valid source profile when one is present. Choosing Assume sRGB for an external Paste gives the existing target-space conversion path a defined source interpretation; Leave Untagged inserts raw components as before.

## Persistence and compatibility

`DocumentColourState` schema 2 stores the input-profile status, selected untagged policy, whether the policy was applied and the original profile fingerprint. Public project format remains 15 and private residency snapshot format remains 16 because both already carry a versioned colour-state object.

Schema-1 states from 0.11.0a migrate to explicit legacy/unknown input metadata. Versions 1–14 retain Legacy V1 processing and are not converted or retagged.

## Safety

- ICC payloads are bounded to 16 MiB.
- Container inspection is bounded to 64 MiB and never controls decoded pixel allocation.
- Invalid profile declarations produce warnings but do not discard image pixels.
- Profile assignment never rewrites RGB or Alpha values.
- Original profile fingerprints and colour-state identity participate in normal persistence/cache semantics.
