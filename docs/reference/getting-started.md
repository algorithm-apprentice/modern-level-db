# Getting Started

[User reference](README.md)

This guide builds and consumes Modern LevelDB from source. Commands run from
the repository root unless a section says otherwise.

## Prerequisites

| Requirement | Current boundary |
|---|---|
| CMake | 3.25 or newer |
| C++ | C++23-capable compiler and standard library |
| Build tool | Ninja is used by repository presets |
| Languages | A C compiler may be required by fetched compression dependencies |
| Python | A Python 3 interpreter is required by the default repository tool tests; Python 3.9+ is required by optional benchmark/profiling/fuzz tooling |
| Dependencies | First configure may download pinned sources; use existing FetchContent overrides for offline builds |
| Linux/macOS | Admitted POSIX backend |
| Windows | Windows 10/11 desktop, native x64 MSVC, local fixed NTFS |

Windows commands require an **x64 Native Tools Command Prompt** or another
shell after `vcvars64.bat` has initialized the same process environment.

The repository currently provides a CMake target for source-tree consumption.
It does **not** install a package configuration for `find_package`.

## Build the repository

### Linux or macOS

```bash
cmake --preset dev-debug
cmake --build --preset dev-debug
ctest --preset dev-debug
```

### Windows x64/MSVC

```text
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

## Consume with `add_subdirectory`

Create a consumer with both C and C++ enabled:

```cmake
cmake_minimum_required(VERSION 3.25)
project(example LANGUAGES C CXX)

set(MODERN_LEVELDB_BUILD_TESTS OFF)
set(MODERN_LEVELDB_BUILD_TOOLS OFF)
add_subdirectory(path/to/modern-level-db modern-leveldb)

add_executable(example main.cc)
target_link_libraries(example PRIVATE modern_leveldb::modern_leveldb)
```

The parent may provide the pinned compression/checksum dependencies through
the repository's supported CMake targets or FetchContent overrides. Public
Modern LevelDB headers do not expose codec headers.

## Open and use a database

```cpp
#include <filesystem>
#include <iostream>
#include <utility>

#include "modern_leveldb/db.h"

int main() {
  modern_leveldb::Options options;
  options.create_if_missing = true;

#if defined(_WIN32)
  // Required on every owned native Windows open. See the durability guide.
  options.allow_weak_namespace_durability = true;
#endif

  auto opened = modern_leveldb::Database::Open(
      options, std::filesystem::path{"example-db"});
  if (!opened.has_value()) {
    std::cerr << opened.error().ToString() << '\n';
    return 1;
  }

  modern_leveldb::Database database = std::move(*opened);
  auto written = database.Put(
      modern_leveldb::AsBytes("key"), modern_leveldb::AsBytes("value"));
  if (!written.has_value()) {
    std::cerr << written.error().ToString() << '\n';
    return 1;
  }

  auto value = database.Get(modern_leveldb::AsBytes("key"));
  if (!value.has_value()) {
    std::cerr << value.error().ToString() << '\n';
    return 1;
  }
  if (value->has_value()) {
    std::cout << modern_leveldb::AsStringView(**value) << '\n';
  }
}
```

Run the executable from a directory where `example-db` is disposable. A
database is not safe to move, edit, or inspect through live files while open.

## Next steps

- Configure API behavior in [API and options](api-and-options.md).
- Read the platform-specific guarantees before choosing sync or mappings:
  [Platform support and durability](platform-support-and-durability.md).
- Use the offline tool through [Storage diagnostics](storage-diagnostics.md).
