# 0.12.0c — Export Profiles and Naming Templates

## Scope

This stage extends the existing single-document, single-output colour-managed export dialog. It does not add the production multi-output editor, queue, per-output resize, folder-wide batch processing or automation.

## Export-profile contract

`ExportProfileData` has payload schema version 1 inside the existing version-2 `vfxphotolab-preset` envelope with kind `export-profile`. A profile stores:

- output format (`png`, `jpg`, `tga`, `tiff`, `webp` or `bmp`);
- 8-bit or supported 16-bit integer encoding;
- no dither or the existing deterministic 64 × 64 blue-noise RGB dither;
- preserve-Alpha-when-supported or explicit flatten-to-matte;
- conversion/keep-working-RGB mode;
- the complete `OutputColourSettings` descriptor, including ICC/OCIO target, rendering intent, black-point compensation and ICC embedding preference;
- format quality;
- matte colour;
- filename template.

Profiles are application-global and use `QStandardPaths::AppDataLocation/presets/export-profile`. They do not enter `.vfxphoto`, Hot/Warm/Cold snapshots, Undo, render identity or processing caches. Built-ins are deterministic and immutable. User profiles use the shared manager for create, update from current, rename, duplicate, category/tags, favourites, recents, import/export and delete.

## Built-in profiles

The initial built-ins are Web PNG sRGB 8-bit, Web JPEG sRGB 8-bit, PNG working-space master 16-bit, TIFF working-space master 16-bit and TGA working-space Alpha 8-bit. Writer availability remains runtime-dependent; a profile cannot bypass the existing Qt/TGA writer checks.

## Filename templates

Templates resolve only the filename stem inside the directory already selected by the ordinary Export action. They cannot change directories. Supported tokens are:

- `{document}`
- `{profile}`
- `{format}`
- `{bit_depth}`
- `{width}` and `{height}`
- `{working_space}` and `{output_space}`
- `{date}` and `{time}`

`{{` and `}}` emit literal braces. One UTC timestamp is captured when the export dialog opens, so preview and accepted output remain deterministic even when date/time tokens are used.

Validation rejects unknown tokens, unmatched braces, empty names, path separators, control characters, Windows-forbidden characters, reserved Windows device names, trailing spaces/full stops and overlong resolved stems. Document/profile/colour-space token values are sanitised as portable filename components. The chosen format supplies the extension separately.

## Quick-export compatibility

`File → Export` still begins with the existing save-file chooser and exports one flattened document. The advanced settings dialog now offers a profile combo and **Manage…** without adding multi-output controls. A profile may change the format and resolved filename while keeping the selected directory. The resolved writer is revalidated and a second overwrite confirmation is shown when the template targets a different existing file.

The existing full-resolution render, working-to-output transform, precision conversion, blue-noise path, cancellation token and atomic writer remain authoritative. Explicit flattening is now available for Alpha-capable formats; hidden RGB is preserved whenever Alpha is preserved, and matte flattening still occurs after output-space conversion.

## Compatibility boundary

This stage does not change project format 15, private residency schema 16, colour-state schema 4, adjustment schema 10 or vector schema 7. No old export setting is reinterpreted. Per-format `QSettings` defaults remain the fallback whenever no profile is selected.
