# Native Platform Final Completion

## Scope

Final every-definition audit for `src/platform`. Source baseline:
`52ab6f449d56d5e7774029f2c6e9ec7a652455cb`.

A fresh tree-sitter scan records 260 C++ definitions and three lambdas across 14
files:

| File | Definitions | Lambdas |
|---|---:|---:|
| `background_executor.h` | 10 | 0 |
| `clock.cc` | 3 | 1 |
| `clock.h` | 6 | 0 |
| `file_system.cc` | 1 | 0 |
| `file_system.h` | 31 | 0 |
| `mapped_read_limiter.h` | 4 | 0 |
| `path.cc` | 1 | 0 |
| `path.h` | 0 | 0 |
| `posix_file_system.cc` | 85 | 0 |
| `posix_file_system.h` | 0 | 0 |
| `serial_executor.cc` | 8 | 2 |
| `windows_file_system.cc` | 92 | 0 |
| `windows_file_system.h` | 0 | 0 |
| `windows_file_system_internal.h` | 19 | 0 |

The parser reports two missing `type_identifier` nodes in `clock.h` and one
recovery node for `FileLock::~FileLock() = 0` in `file_system.h`. Manual review
confirms that these are pure-virtual declarations, not omitted definitions; the
out-of-class `FileLock` destructor definition is counted separately.

The complete mechanical index is retained as
`native-platform-final-index.json` in the session evidence.

## Delivered lifecycle groups

Earlier accepted work remains part of this final ledger:

- path resolution, directory admission, regular opening, mapping admission,
  directory enumeration and positioned-read completion from N01-N09;
- writable, close, lock and directory-sync lifecycles from #265 and #266;
- scheduling, clock and mapping-budget support from #267;
- mapped-region/view ownership from #268;
- sequential and copied-positioned read ownership from #269.

These deliveries overlap by design. Counts from individual batch ledgers must
not be added to infer coverage. This document re-evaluates every current
definition and helper once at the final source revision.

## Final remaining implementation frontiers

### Windows native error composition

`FileError` currently builds the operation/path/Win32 prefix, queries and
converts the system message, and classifies the error code in one function.
Extract:

1. appending an available system message;
2. classifying one completed message as NotFound, InvalidArgument,
   NotSupported or Io;
3. the existing `FileError` composition.

Capture a `FileError` source location and pass it into the classification leaf
so refactoring does not move diagnostics into the new helper. Preserve exact
message text, code priority, conversion failure behavior and allocations.

### POSIX existing-directory classification

`CreateDirectory` should keep path validation and `mkdir` in the public
operation, then delegate only `EEXIST` inspection:

- `stat` the existing path;
- accept a real directory;
- reject a non-directory with the existing InvalidArgument message.

Pass a `CreateDirectory` source location for the InvalidArgument result.
Other create errors keep the existing `FileError` path.

### Windows existing-directory classification

After `CreateDirectoryW` reports `ERROR_ALREADY_EXISTS`, delegate:

- native attribute acquisition and its I/O error;
- acceptance of a directory;
- rejection of a non-directory with the existing InvalidArgument message.

Pass a `CreateDirectory` source location for the InvalidArgument result. Do not
repeat path conversion or change `GetLastError` capture order.

## Complete disposition

After the three frontiers above:

- interface special members and default views are ownership/availability leaves;
- `CreateDefaultFileSystem` is the compile-time platform factory composition,
  with Windows directory preparation explicit and POSIX directory identity
  preserved;
- path conversion and native request clamps are boundary leaves;
- descriptor/handle, directory, mapping, lock and executor owners are RAII
  leaves with reviewed move/destruction invariants;
- read, write, sync, close, lock, mapping, enumeration and namespace public
  methods are compositions over named platform leaves;
- native syscall retry loops, encoding loops, path-component scans and
  cancellation drains are bounded algorithmic leaves;
- the three support lambdas are single-expression worker/wait predicates.

Do not add wrappers around direct one-call leaves. Do not merge POSIX and
Windows error or resource policies into a cross-platform helper.

## Characterization and validation

Before production edits:

- extend the POSIX existing-file `CreateDirectory` test with exact message and
  `CreateDirectory` source function;
- add the equivalent Windows existing-file test;
- extend Windows missing-file characterization with exact `FileError` source
  function;
- retain namespace, error-stage, factory and all previously selected native
  identities.

Windows tests run locally on unchanged production. POSIX source-context and
namespace identities are committed before edits and verified by Hosted GCC 13.

After each topic commit, run the affected platform selection. The final native
scan must still account for every definition and lambda, including new helpers.
Final delivery requires exact-head 20/20 CI, 100% changed-code branch coverage,
actual POSIX/Windows identity evidence and merged-path equality.

## Review focus

Review error source ownership, `GetLastError`/`errno` capture timing, code/message
priority, no extra path conversions, existing-directory native call order and
complete final-ledger coverage. Reject helpers that merely rename a single
expression or hide platform policy.
