# ADR-0071: Four-Space C++ Formatting

- Status: Accepted
- Date: 2026-10-10

## Context

The existing `.clang-format` inherits two-space indentation from the Google
style, with a 100-column limit, attached opening braces, left-aligned pointer
markers, and case-sensitive include sorting.

The project owner requests four-space indentation, informed by a mature
open-source project's formatter configuration. This is a layout change, not
a change to naming, APIs, storage formats, or runtime behavior.

## Reference

[Catch2's `.clang-format` at commit
`04382af4c640d801d332191528d11bd7ae5819f3`](https://github.com/catchorg/Catch2/blob/04382af4c640d801d332191528d11bd7ae5819f3/.clang-format)
provides a concrete C++ reference for:

- `IndentWidth: 4`.
- `TabWidth: 4` and `UseTab: Never`.
- `AccessModifierOffset: -4`, aligning access labels with the class declaration.
- `DerivePointerAlignment: false` and `PointerAlignment: Left`, which already
  match Modern LevelDB.

Catch2 also indents namespaces and inserts spaces inside parentheses. Those
preferences are not required by this request and are not adopted. Referencing
an established configuration does not mean replacing every existing convention.

## Decision

Retain `BasedOnStyle: Google` and explicitly set:

| Setting | Value | Purpose |
|---|---|---|
| `IndentWidth` | `4` | Four spaces per block level |
| `ContinuationIndentWidth` | `4` | Four-space relative continuation indentation |
| `AccessModifierOffset` | `-4` | Access labels aligned with the class declaration |
| `TabWidth` | `4` | A defined four-column tab width |
| `UseTab` | `Never` | Emit spaces rather than tabs |

The existing 100-column limit, attached braces, pointer/reference alignment,
include sorting, namespace layout, and argument-alignment policy remain
unchanged. Continuation indentation was already four spaces; spelling it out
makes the four-space variant explicit. Arguments may still align with the
first argument rather than always starting at a multiple of four columns.

`.clang-format` remains the formatting source of truth.
[Code Style](../development/code-style.md) explains the convention and its
reference. Identifier naming and clang-tidy policy do not change.

## Rollout and validation

Update the style guide and this decision before changing the configuration.
Apply the formatter once to all tracked C++ sources and headers in `include`,
`src`, `tests`, `benchmarks`, `fuzz`, and `tools`, including native Windows
sources. Do not format downloaded dependencies, generated files, or build
directories.

Update the live C++ examples in the user reference and learning guides as
well. Historical ADR snippets remain records of their original decisions
and are not reformatted.

Keep this migration separate from behavior changes. Keep coverage-exclusion
markers on their associated code lines; shorten their explanations only where
needed to prevent the annotated statement from wrapping. Preserve literal
values, deliberately ordered Windows includes, and the row layout of golden
byte fixtures in `clang-format off` regions; adjust only their enclosing
source indentation as needed.

Check the effective formatter settings, require a clean formatter dry run on
all tracked C++ files, compare significant C++ tokens and include inventories,
and build and run the existing development tests. Run the existing
documentation gate for the new ADR index entry and live examples. Require
the existing cross-platform PR checks before merging. No new formatter
dependency, CI job, or test framework is needed for this convention change.

## Consequences

- Four-space indentation gives nested control flow a wider visual separation.
- Access labels do not acquire an incidental three-space indent from Google's
  default access-modifier offset.
- Deeper nesting can wrap earlier under the unchanged 100-column limit.
- A one-time mechanical diff avoids a mixture of two- and four-space source
  styles. Future behavior changes format only the files they touch.
