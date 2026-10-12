# Native Mapping Ownership Completion

## Scope

Hardest-first completion of mapped random-access resource ownership on POSIX
and Windows. Source baseline:
`c233983dc21929a396058d9dfdb8599310c82fe4`.

The original mapping lifecycle contains 40 C++ definitions and no lambdas:

| Platform | Definitions | Included responsibilities |
|---|---:|---|
| POSIX | 18 | Copied fallback, size admission, process budget, pending mapping guard, mapped reader, filesystem constructors and `OpenRandomAccess`. |
| Windows | 22 | Process limiter, copied fallback, size admission, pending mapping guard, mapped reader, section/view/handle transaction, filesystem constructors and `OpenRandomAccess`. |

The delivered N06/N07 slices already name mapping eligibility and Windows
section/view acquisition. This batch completes ownership transfer and teardown;
it does not redesign admission, fallback or native call ordering. Benchmarks
remain out of scope.

## Shared ownership model

Each platform receives one private movable mapping owner. It owns the admitted
mapping slot immediately and may additionally own a mapped view. The pending
transaction and the published random-access file use the same owner, so cleanup
is not duplicated.

The owner:

- is noncopyable and nonassignable;
- has a `noexcept` move constructor that transfers the slot, view and extent;
- marks the moved-from object inactive by moving out its limiter/budget;
- exposes only view data, extent and one mapping-publication operation;
- releases exactly one admitted slot during destruction;
- remains owned by the final random-access file after publication.

The final reader stores the owner directly. Its destructor is defaulted; the
platform owner performs unmap and slot release. Allocation failure while
constructing the final reader leaves the pending owner active, so it still
rolls back the view and slot.

Do not create a cross-platform mapping class. POSIX and Windows intentionally
have different teardown failure policies and dependencies.

## POSIX contracts

Mapping eligibility completes before slot acquisition. A missing, zero,
unrepresentable or mismatched size falls back to copied reads without consuming
a slot. Slot exhaustion also falls back.

After admission, the pending owner exists before `mmap`. A real `mmap` failure
falls back to the copied reader and releases the slot. Once a view exists,
allocation failure unmaps it and releases the slot. Success moves the same owner
into `PosixMmapRandomAccessFile`; the original descriptor closes independently.

Mapped bytes remain valid after filesystem destruction and file unlink. Final
teardown ignores `munmap` status, matching the current best-effort POSIX policy,
then releases the mapping slot. No runtime assertion or diagnostic branch is
added.

## Windows contracts

Windows validates the size hint, acquires a slot, creates the pending owner and
then queries the native file size. A size-query failure remains an I/O error; a
size mismatch falls back to copied reads and releases the slot.

The native transaction order remains:

1. create the mapping section;
2. map the view;
3. close the section handle;
4. close the file handle;
5. publish the mapped reader.

Every failure preserves the existing error priority and native call prefix.
Once a view exists, rollback follows the existing scope-unwind order: explicit
close attempts retain priority, the pending owner unmaps and releases its slot,
and any still-owned acquisition handle closes through its current RAII owner.
The mapping owner retains `WindowsFileOperations`, so a published view outlives
its originating filesystem.

Windows unmap failure remains fatal. The owner must call `std::terminate`
before releasing the limiter slot, preserving the rule that a view whose
teardown failed is never treated as recyclable.

## Definition disposition

Copied-reader construction, size conversion/query, process budget creation and
filesystem constructor delegation remain responsibility leaves.
`OpenRandomAccess` remains the admission composition delivered by N06/N07.
Section creation, view mapping and ordered handle closure remain separate
Windows leaves.

The old pending guards and final-reader destructors are replaced by:

- one POSIX mapped-region owner;
- one Windows mapped-view owner;
- `noexcept` owner transfer into each final reader;
- defaulted final-reader destruction.

All original definitions plus the new owner special members/accessors
participate in the final ledger.

## Characterization and validation

Before production edits, preserve these 17 native identities:

- six POSIX mapping tests covering stable views, shared slot release, fallback,
  unlink survival, pending-guard behavior and owner lifetime;
- ten Windows mapping tests covering owner lifetime, concurrent reads, table
  integration, slot reuse, admission, concurrent opens, rollback traces,
  publication traces and namespace mutation;
- the Windows death test for failed owned-view unmap.

Windows fault-operation traces must remain exact. Original Linux/POSIX
identities come from the unchanged Hosted GCC 13 run for #267; original Windows
identities are also captured locally before edits.

Original evidence at `c233983dc21929a396058d9dfdb8599310c82fe4` is 11/11
Windows Debug mapping identities and 6/6 Hosted GCC 13 POSIX mapping identities.

After each platform-specific production commit, run its native mapping
selection. Final selected identities must exactly match the characterized set.
Run Windows Debug/Release, GCC/Werror owned objects and complete Hosted CI on
the reviewed PR head.

## Review focus

Review move construction before publication, allocation-failure rollback,
moved-from inactivity, exact handle/unmap unwind ordering,
unmap-before-slot-release, Windows termination before release, POSIX
best-effort teardown and exact fallback/error behavior. Reject boolean commit
flags, duplicate cleanup bodies or a shared abstraction that erases platform
policy.
