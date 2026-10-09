# Building and Testing

[Development guide](README.md)

Commands run from the repository root unless stated otherwise. Repository
presets use Ninja and require CMake 3.25 or newer.

## Prerequisites

- A C++23-capable compiler and standard library.
- A C compiler when pinned C dependencies are built from source.
- Ninja.
- Python 3; Python 3.9+ for benchmark/profiling/fuzz tooling.
- Network access on first configure unless pinned dependency source overrides
  or parent-provided targets are available.

On Windows use an x64 MSVC developer environment. Keep `vcvars64.bat`,
configure, and build commands in the same process.

## Core presets

| Purpose | Configure/build/test preset |
|---|---|
| Linux/macOS Debug development | `dev-debug` |
| Linux/macOS Release | `release` |
| Portable extended model/compatibility/crash tests | `compatibility` |
| Windows x64 Debug | `windows-debug` |
| Windows x64 Release | `windows-release` |
| Windows extended recovery/compatibility | `windows-compatibility` |

Example:

```bash
cmake --preset dev-debug
cmake --build --preset dev-debug
ctest --preset dev-debug
```

Windows x64/MSVC:

```text
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Every test preset uses `noTestsAction=error`. An empty, stale, or mistyped
selection is not a passing validation.

## Focused test selection

List matching tests before a focused run:

```bash
ctest --preset dev-debug -N -R 'Pattern'
ctest --preset dev-debug -R 'Pattern' --output-on-failure
```

Common labels include:

- `unit`
- `cmake`
- `model`
- `compatibility`
- `crash`
- `benchmark`
- `performance`
- `fuzz`

Run the smallest tier that proves the change, then expand when shared code,
public behavior, CMake integration, persistent formats, or platform selection
is affected.

## Extended verification

Linux/macOS:

```bash
cmake --preset compatibility
cmake --build --preset compatibility --target modern_leveldb_extended_tests
ctest --preset compatibility
```

Windows:

```text
cmake --preset windows-compatibility
cmake --build --preset windows-compatibility --target modern_leveldb_extended_tests
ctest --preset windows-compatibility
```

These tiers include seeded models, golden/cross-open compatibility,
deterministic fault/power-loss models, and owned process-crash evidence.
Process-crash tests are not real power-loss certification.

## Sanitizers, coverage, and fuzzing

```bash
cmake --preset asan
cmake --build --preset asan
ctest --preset asan

cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan

cmake --preset coverage
cmake --build --preset coverage
ctest --preset coverage

cmake --preset fuzz
cmake --build --preset fuzz
ctest --preset fuzz
```

Use the compiler/tool versions selected by CI when reproducing a gate. Do not
apply GCC coverage/sanitizer flags to native MSVC presets.

## Dependency isolation

The repository fetches pinned Snappy, Zstd, CRC32C, GoogleTest, Google
Benchmark, and LevelDB reference sources where required. Parent targets and
FetchContent source overrides are accepted only through documented CMake
boundaries; tests reject predeclared/provider-populated dependencies where
isolation is part of the contract.

Codec headers do not enter the public API. Optional benchmark/reference
targets stay private to their build.

## Subproject behavior

Tests default on only for a top-level build. A parent can disable them with:

```cmake
set(MODERN_LEVELDB_BUILD_TESTS OFF)
add_subdirectory(path/to/modern-level-db)
```

`BUILD_TESTING=OFF` also disables test targets and their dependencies.
Top-level command-line tools default on for admitted platforms; set
`MODERN_LEVELDB_BUILD_TOOLS=OFF` when embedding without them.
