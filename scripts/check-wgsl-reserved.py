#!/usr/bin/env python3
"""Reject known-invalid WGSL source before wgpu-native sees a shader module."""

from __future__ import annotations

import re
import sys
from pathlib import Path

RESERVED = frozenset(
    """
NULL Self abstract active alignas alignof as asm asm_fragment async attribute auto
await become cast catch class co_await co_return co_yield coherent column_major
common compile compile_fragment concept const_cast consteval constexpr constinit
crate debugger decltype delete demote demote_to_helper do dynamic_cast enum explicit
export extends extern external fallthrough filter final finally friend from fxgroup
get goto groupshared highp impl implements import inline instanceof interface layout
lowp macro macro_rules match mediump meta mod module move mut mutable namespace new
nil noexcept noinline nointerpolation non_coherent noncoherent noperspective null
nullptr of operator package packoffset partition pass patch pixelfragment precise
precision premerge priv protected pub public readonly ref regardless register
reinterpret_cast require resource restrict self set shared sizeof smooth snorm static
static_assert static_cast std subroutine super target template this thread_local throw
trait try type typedef typeid typename typeof union unless unorm unsafe unsized use
using varying virtual volatile wgsl where with writeonly yield
""".split()
)

IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
RAW_WGSL = re.compile(r'R"WGSL\((.*?)\)WGSL"', re.DOTALL)
MULTI_COMPONENT_SWIZZLE_ASSIGNMENT = re.compile(
    r"\b[A-Za-z_][A-Za-z0-9_]*\.((?:[rgba]{2,4})|(?:[xyzw]{2,4}))\s*=(?!=)"
)


def strip_comments(text: str) -> str:
    """Remove WGSL line and nested block comments while preserving line count."""
    output: list[str] = []
    index = 0
    block_depth = 0
    line_comment = False
    while index < len(text):
        if line_comment:
            if text[index] in "\r\n":
                line_comment = False
                output.append(text[index])
            else:
                output.append(" ")
            index += 1
            continue
        if block_depth:
            if text.startswith("/*", index):
                block_depth += 1
                output.extend((" ", " "))
                index += 2
            elif text.startswith("*/", index):
                block_depth -= 1
                output.extend((" ", " "))
                index += 2
            else:
                output.append(text[index] if text[index] in "\r\n" else " ")
                index += 1
            continue
        if text.startswith("//", index):
            line_comment = True
            output.extend((" ", " "))
            index += 2
            continue
        if text.startswith("/*", index):
            block_depth = 1
            output.extend((" ", " "))
            index += 2
            continue
        output.append(text[index])
        index += 1
    return "".join(output)


def failures_for_text(label: str, text: str) -> list[str]:
    clean = strip_comments(text)
    failures: list[str] = []
    for match in IDENTIFIER.finditer(clean):
        token = match.group(0)
        if token not in RESERVED:
            continue
        line = clean.count("\n", 0, match.start()) + 1
        failures.append(f"{label}:{line}: WGSL reserved word used as a token: {token}")

    for match in MULTI_COMPONENT_SWIZZLE_ASSIGNMENT.finditer(clean):
        line = clean.count("\n", 0, match.start()) + 1
        failures.append(
            f"{label}:{line}: WGSL cannot assign to multi-component swizzle .{match.group(1)}; "
            "construct and assign the complete vector instead"
        )
    return failures


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    failures: list[str] = []

    for shader in sorted((root / "shaders").glob("*.wgsl")):
        failures.extend(failures_for_text(str(shader.relative_to(root)), shader.read_text()))

    for source in sorted((root / "src").rglob("*.cpp")):
        text = source.read_text()
        for index, match in enumerate(RAW_WGSL.finditer(text), start=1):
            line = text.count("\n", 0, match.start()) + 1
            label = f"{source.relative_to(root)}:WGSL-block-{index}@{line}"
            failures.extend(failures_for_text(label, match.group(1)))

    if failures:
        print("WGSL source validation failed:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1

    print("WGSL source validation passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
