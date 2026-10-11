# POSIX mapping admission and directory traversal composition

## Scope and platform evidence

Design N06 and the POSIX half of N07 in the
[production composition plan](production-function-composition-plan.md).
Change posix_file_system.cc, direct POSIX characterizations and this document.
No Windows mapping/enumeration, public interface, native fault framework,
namespace durability, locking, read/write buffering or coverage-policy changes.
This source scope is independent of Windows directory implementation #214.

The current workstation is Windows. Its WSL Ubuntu registration exists, but
the attempted native build-tool probes found no working cmake/GCC 13/Ninja
toolchain. Windows tests do not execute this POSIX implementation.
Do not present Windows build success as original/final POSIX parity.
After design acceptance, publish independently reviewed characterization-only
commits and require native Linux/macOS CI to build and execute the new cases
on unchanged production before adding the extraction commits to the same PR.
Verify registered case names and successful native test results from actual
CI evidence, not just a skipped job or test source presence.
If a local native toolchain later becomes available, run the same selected
original/final suite and retain exact JUnit sets; do not overwrite existing
Windows build directories.

## Mapping eligibility and copied ownership

OpenRandomAccess retains OpenDescriptor and its original error copy.
The disabled-budget guard still precedes hint validation and fstat.

`MappingLength(descriptor, expected_size)` checks present hint, nonzero size
and size_t representability in original order, then invokes
`MatchesMappingSize(descriptor, expected_size)`. Return optional<size_t>;
empty intentionally means the original copied-read fallback, not an I/O error.
No extra expected-size/platform bound or regular-file type validation.

`MatchesMappingSize` retains one fstat, original failure fallback, negative-size
fallback and uint64 equality comparison. Keep the existing coverage annotations
on the same originally uninjectable/32-bit conditions when moving them; add no
new exclusions. Do not turn POSIX fstat/mmap fallback into Windows-style fatal
mapping errors or acquire a slot before size inspection.

`OpenCopiedRandomFile(descriptor, path)` names allocation of the copied reader
and Release only after successful construction, using the existing
unique_ptr<RandomAccessFile> local envelope. Borrow the scoped descriptor owner;
do not transfer it before construction or return a borrowed file/path.
This replaces the captured fallback lambda with a named owner-transfer
operation, not a single-call forwarding wrapper.

OpenRandomAccess composes descriptor acquisition, disabled/hint/size eligibility,
slot admission, unchanged pending mapping transaction and copied/mapped outcome.
Preserve Acquire after exact size validation, PendingMmap construction after
Acquire and mmap after pending ownership.
Mapping failure must invoke copied construction while the parent pending guard
is still alive, as originally. Do not return an empty optional from a new mapping
helper that destroys the guard before fallback construction.
Successful mapped-object construction precedes pending Commit; the descriptor
still closes at parent return rather than transferring into the mapped object.
Exceptions retain original descriptor/slot/view cleanup order.
The actual mmap transaction and mapped-object implementation remain recursive
frontiers; this batch only decomposes eligibility and copied ownership.

## Directory entry traversal

ListDirectory keeps path validation, opendir and ScopedDirectory in the parent.
`ReadDirectoryChildren(directory, children)` owns the readdir loop and returns
the captured integer errno at its stopping point. Before every readdir set
errno to zero; null captures errno immediately and stops. A short function
returning an integer intentionally delays Error construction until after the
original explicit closedir in the parent.
Do not introduce Status/Error allocation, new exceptions or a retry in the
native read-error branch.

`AppendDirectoryChild(children, entry)` preserves copying d_name into a string
before checking the two exact dot names, then emplaces accepted names as
filesystem::path. Do not replace this with UTF-8 conversion, sort, deduplicate,
inspect entry types, follow symlinks or change the native enumeration order.
The copied name/path must not borrow the readdir buffer after another read.

The read loop composes one native read/stop transition and the child-name
publication operation. Allocation exceptions unwind the still-owned directory,
as originally, without explicit-close error replacing the exception.
After traversal the parent releases the DIR owner, performs closedir exactly
once, then reports captured read_error before close failure. Preserve original
close-error errno capture timing and FileError messages. No partial success on
read/close failure and no destructor reclose after explicit release.
Windows enumeration remains separate because its native stop/error rules differ.

## Definition responsibility ledger

| Definition | Responsibility or composition boundary |
|---|---|
| MatchesMappingSize | One fstat/exact-size eligibility decision with original fallback policy. |
| MappingLength | Ordered hint/representation/observed-size admission composition. |
| OpenCopiedRandomFile | Allocate copied owner then transfer the descriptor. |
| OpenRandomAccess | Acquire, admit and compose original copied/mapped ownership transaction. |
| AppendDirectoryChild | Own and publish one native child name excluding exact dot entries. |
| ReadDirectoryChildren | Native read/stop and child-publication traversal. |
| ListDirectory | Validate, acquire, traverse, close and prioritize errors. |

Predicates, descriptor helpers, mapping transaction, PendingMmap, mapped/copy
reader internals and other native operations remain recursive review frontiers.
This seven-definition ledger is not certification of all POSIX file-system code.

## Original characterization and delivery

Existing native tests cover exact stable mapped views, missing/zero/mismatched
hints, shared count exhaustion/release, unlink lifetime, copied reads, namespace
operations and RAII. Before extraction add:

- A one-slot guard matrix with no hint, zero hint and both size mismatches kept
  alive as copied readers while a valid exact-size open still maps. Exhausted
  exact-size fallback stays readable after the occupying map is released; a
  new exact open then maps. Assert actual bytes and TryReadView outcomes.
- File objects outliving their originating filesystem: retained mapped view
  remains valid after unlink; copied fallback retains its descriptor and reads
  after filesystem destruction. Budget reuse follows owning map destruction.
- Directory names with dot-prefix, spaces, non-UTF-8 bytes, regular files,
  subdirectory and symlink. Assert exact native-name set with no dot/dot-dot,
  preserve all accepted names without decoding and do not depend on order.
  Enumerate repeatedly to expose leaks; retain invalid/missing/nondirectory
  failures and assert original typed error/path attribution.

Real post-open fstat/mmap failure and post-entry readdir/closedir faults lack
an existing deterministic seam. Preserve original code and coverage annotation
boundaries and state these limits; do not introduce seams or claim injected
oracles. Native CI execution of unchanged-production cases is mandatory before
source edits, given this workstation's toolchain limit.
Implement mapping eligibility/ownership and directory traversal as separate
topic commits. Require the same native cases and exact successful before/after
selection evidence including PosixFileSystemTest, coupled table/recovery tests
and actual suite names; no failure/error/skipped cases.
Independent combined review precedes the implementation push, and all reviewed
exact-head CI checks precede normal merge and persistent-content verification.
Formatting/docs/whitespace and existing coverage thresholds apply unchanged.
