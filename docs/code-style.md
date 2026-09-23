# Code Style

This guide defines the shared C++ style for Modern LevelDB. It applies to
production code, tests, and examples. Use English for identifiers, comments,
documentation, ADRs, and commit messages.

## Naming

PascalCase capitalizes the first letter of each word without separators.
snake_case uses lowercase words separated by underscores.

| Category | Convention | Examples |
|---|---|---|
| Types and type aliases | PascalCase | `Error`, `ErrorCode`, `ByteView`, `Result` |
| Functions and ordinary member functions | PascalCase | `AsBytes`, `ConsumeFixed32`, `ToString` |
| Simple value accessors | snake_case | `code()`, `message()`, `location()` |
| Scoped enum members | PascalCase, no `k` prefix | `ErrorCode::NotFound`, `ErrorCode::Corruption` |
| Named compile-time constants (`constexpr`) | PascalCase, no `k` prefix | `PayloadBits`, `MaxBytes`, `EncodedSize` |
| Parameters and local variables | snake_case | `input`, `remaining_bits`, `maximum_payload` |
| Private non-static data members | snake_case with a trailing underscore | `code_`, `message_`, `location_` |
| Template type parameters | PascalCase or a conventional single capital letter | `UInt`, `T` |
| Namespaces | snake_case | `modern_leveldb` |
| C++ filenames | snake_case with `.h` or `.cc` | `result.h`, `coding.cc`, `coding_test.cc` |
| Include guards | UPPER_SNAKE_CASE with a trailing underscore | `MODERN_LEVELDB_BASE_RESULT_H_` |
| GoogleTest suites and cases | PascalCase | `CodingTest`, `RejectsOverflowingVarint32` |

Do not add a `k` prefix to enum members or compile-time constants. The
`constexpr` qualifier and enum scope already communicate their roles.
Runtime `const` locals still use snake_case; immutability alone does not make
a local variable a named compile-time constant.

```cpp
constexpr std::size_t PayloadBits = 7;
std::size_t index = 1;
const std::size_t shift = index * PayloadBits;
```

Simple accessors intentionally follow the value-like style common in the C++
standard library. Use `error.code()` rather than `error.Code()`, while
operations such as `error.ToString()` use PascalCase.

This is a project-specific C++ convention, not a wholesale adoption of C#
or Google naming rules. Keep standard-library and external API spellings
unchanged. Diagnostic strings such as `not_found`, comparator identities, and
persistent-format identifiers are not governed by C++ identifier naming rules.

## Formatting

[`.clang-format`](../.clang-format) is the source of truth for formatting:

- Google-based layout with two-space indentation.
- A 100-column limit.
- Opening braces on the declaration or control-flow line.
- Left-aligned pointer and reference markers, such as `Error*` and `ByteView&`.
- Case-sensitive include sorting.

Use the formatter on changed C++ files rather than manually adjusting layout.
For example, from the repository root:

```bash
clang-format -i src/base/coding.cc tests/unit/base/coding_test.cc
```

On macOS, `xcrun clang-format` can be used when the formatter is provided by
Xcode. Avoid reformatting unrelated files in a behavior or naming change.

`clang-format` controls layout, not identifier naming. The current
[`.clang-tidy`](../.clang-tidy) configuration also does not enforce the naming
table; naming consistency must be checked during review.

## API and comments

Follow [ADR-0004](adr/0004-errors-ownership-and-runtime.md) for the error,
ownership, and runtime model.

- Use `Result<T>` for fallible operations returning a value, and `Status` for
  fallible operations without a success payload.
- Mark functions whose result must not be ignored with `[[nodiscard]]`.
- Use `noexcept` only when the implementation and its callees uphold that
  guarantee.
- Use RAII for ownership and document borrowed-view lifetimes.
- Explain non-obvious contracts and invariants in comments; do not narrate
  straightforward code.

## Review and changes

Use behavior-oriented test names, as in
`CodingTest.RejectsTruncatedVarintWithoutConsumingInput`.
Follow [ADR-0005](adr/0005-test-driven-development.md) for the TDD workflow.
Behavior-preserving internal renames belong to the refactoring step: keep
the existing tests green before and after the change rather than adding
tests that inspect private identifier spellings.

Coverage exclusions follow [ADR-0019](adr/0019-test-coverage-policy.md):
prefer deleting unreachable code, and give every `GCOVR_EXCL_*` marker its
justification in the same comment, for example
`// GCOVR_EXCL_BR_LINE: exhaustive switch over decoded types`.

Keep style changes scoped to the requested change. Update this guide before
introducing a different convention, and retain the sequential pull-request
workflow from [ADR-0006](adr/0006-sequential-pull-request-workflow.md).
