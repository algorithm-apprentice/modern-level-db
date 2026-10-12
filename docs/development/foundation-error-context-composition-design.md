# Foundation Error Context and Primitive Audit

## Status and scope

Proposed recursive follow-up to the production composition plan.
Source baseline: `5da8f8eb9d13f3b8845fb812251e055ece051a67`.
Pending lookup #250 uses independent source/test/design paths.
This is not a whole-production completion claim or benchmark task.

Two small production topics may share one non-draft implementation PR:
first-failure capture versus sticky result projection; optional diagnostic
message append versus parent string construction.
Changed paths: src/base/result_internal.cc, directly dependent
include/modern_leveldb/base/result.h, existing tests/unit/base/result_test.cc,
and this document. No public signature, Error factory default location,
status/exception policy, allocation strategy, diagnostic format or coverage
threshold/exclusion changes. Existing CRC source/header are inspected but
unchanged; the external crc32c dependency is not a refactoring candidate.

## First failure capture and sticky result

Direct source/caller reading establishes eight real native writer callers
across POSIX and Windows. RememberFirstError owns its incoming by-value Status
until function exit. Preserve that parameter and current callers verbatim.
Call file-local CaptureFirstFailure(first_error, status), borrowing both.
The child retains the original left-to-right
!status.has_value() && !first_error.has_value() condition and moves the failed
Error into the empty slot only then. Do not copy the Status parameter, return
a replacement owner, clear its failed alternative, add noexcept, or move the
caller-visible error projection into the child.

The parent then keeps its original first_error presence gate, copied
std::unexpected(*first_error), or successful empty Status.
This named constructor is already the result-materialization primitive; do
not add a pure projection wrapper. The newly latched optional remains installed
if later error-copy allocation throws. The incoming Status stays alive through
that copy, preserving its moved-from payload destruction boundary.
No catch, rollback, fallback or new source_location is introduced.

Do not force native writer Close/Flush/Sync resource ownership through this
helper or rewrite their first-error ordering. Existing native Error locations
come from their factories and must remain the original winning context.
Ordinary execution does not establish allocation-failure counts or physical
move/elision/destruction counts.

## Diagnostic string parent

Error::ToString retains its parent std::string text(ErrorCodeName(code_)).
Call private AppendMessageIfPresent(text) const, then return the same parent
owner. The child preserves the !message_.empty() gate and the two original
append operations (": ", then the complete message_) in order.
It borrows the text and object; no returned staging string, message view copy,
generic escaping framework, extra concatenation allocation or noexcept.
Embedded zero/high bytes remain message bytes, not C strings or escaped output.
Both parent and helper stay inline in the same existing public header.

Factories retain their by-value message and default source_location::current
parameters and direct typed Error construction. Do not move the location default
to a helper call or change the private constructor/member initializer order.
ErrorCodeName's exact seven names and unknown fallback remain unchanged.

## Twenty-one-definition source disposition

The following ledger includes the four selected new/changed definitions and
seventeen directly inspected unchanged definitions. It is bounded to the listed
bodies, not dependent external-library or caller-file certification.

| Definition | Children or responsibility-based stopping reason |
|---|---|
| `RememberFirstError` | Borrowing first-failure capture, then existing sticky Result construction. |
| `CaptureFirstFailure` | Coupled first-empty-slot admission and failed Error ownership transfer. |
| `Error::ToString` | Parent code-name string, optional message append, same owner return. |
| `Error::AppendMessageIfPresent` | Coupled separator/full-message append under its existing nonempty gate. |
| `ErrorCodeName` | One total ErrorCode-to-diagnostic-name classification with stable fallback. |
| `Error::NotFound` | Direct typed Error construction preserving message owner/call-site location. |
| `Error::Corruption` | Direct typed Error construction preserving message owner/call-site location. |
| `Error::InvalidArgument` | Direct typed Error construction preserving message owner/call-site location. |
| `Error::Io` | Direct typed Error construction preserving message owner/call-site location. |
| `Error::NotSupported` | Direct typed Error construction preserving message owner/call-site location. |
| `Error::Busy` | Direct typed Error construction preserving message owner/call-site location. |
| `Error::Aborted` | Direct typed Error construction preserving message owner/call-site location. |
| `Error::code` | Borrowed scalar code projection. |
| `Error::message` | Borrowed complete message view projection; storage remains with Error. |
| `Error::location` | Borrowed original source-location projection. |
| `Error::Error` | Coupled member initialization of complete code/message/location representation. |
| `TakeTrusted<T>` | Existing named expected.value extraction from proven-success owning parameter. |
| `Crc32c` | Existing ExtendCrc32c child with finalized zero seed. |
| `ExtendCrc32c` | Empty identity gate, otherwise existing external CRC extension primitive with borrowed bytes/seed. |
| `MaskCrc32c` | Coupled fixed-width rotate/add storage masking bijection using existing std::rotr primitive. |
| `UnmaskCrc32c` | Coupled subtract/rotate inverse storage transform using existing std::rotl primitive. |

Defaulted/generated Error special members are not new explicit source bodies.
Type aliases and declarations are not definitions. No short-function or complex
algorithm exemption is used; the untouched CRC functions already compose their
named primitives or implement one representation-specific transform.
Existing independent CRC literals/bitwise reference, empty seed identity,
every-bit inverse and guarded unaligned/subspan cases are retained.
Their ordinary execution does not certify external CPU-dispatch variants,
dead reads, physical copies, performance or every other base function.
Other native writer/resource children remain in the every-definition audit.

## Original-source characterization

Existing ResultInternalTest covers all four incoming-status/first-slot states:
empty success, empty failure, occupied success and occupied later failure.
Retain it and ResultTest error codes, default successful Status, typed owning
Result, formatting and factory source-location cases.
Add two public cases before production edits:

1. Full sticky Error context and ownership: long first message, exact code,
   line/file/function location survives later success/different later failure.
   Returned Status copies and retained optional own independent Error messages;
   replacing/resetting one does not mutate earlier results or winning context.
   Compare actual fields and independent long bytes, not only equal code.
   Do not dereference moved-from input payloads or claim allocation failure.
2. Formatter factory/message matrix: all seven literal code-name prefixes with
   empty, embedded-zero/newline/high-byte and long messages; exact complete
   diagnostic strings, unchanged original message and explicit factory location.
   Build expectations from literal prefixes/separator/raw input bytes, not
   production ErrorCodeName/ToString. Keep owned matrix values live, avoid
   GCC 13 range-for temporary-member lifetime assumptions.

Build unit/Windows-lock-child executables and measure coupled anchored suites:
`^(ResultTest|ResultInternalTest|Crc32cTest|WindowsFileSystemTest|PosixFileSystemTest|WalWriterTest|WalIoNativeTest|TableBuilderTest|DatabaseTest)\.`
Native platform classes intentionally differ; measure actual nonempty classes
and do not substitute Windows evidence for POSIX execution.
Local intermediate:
`^(ResultTest|ResultInternalTest|Crc32cTest)\.`
Original/final unique names must match exactly with zero failure/error/skip.
Source-declared counts are navigation, not runtime proof.

Accepted independently reviewed non-draft design precedes original cases;
independent original-case review precedes both production topics;
integrated review precedes non-draft implementation publication.
Preserve existing annotations verbatim/full-text multisets and expose helper
Status ownership/exception boundaries explicitly. All exact reviewed-head CI
checks plus actual hosted Windows Debug selected identities are required before
normal merge, own-path equality, main fast-forward and branch cleanup.
This is a bounded source disposition, not final every-definition certification.

### Independent design review

The bounded read-only reviewer independently traced the proposed responsibilities,
actual native callers, contracts, test oracles and twenty-one-definition ledger.
No significant issue was found. No edits, builds, execution or nested agents
were performed. Implementation, allocator-failure behavior, hosted runtime and
whole-production/every-definition completion remain unverified.
