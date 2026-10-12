# Diagnostic Table Traversal Composition

## Status and scope

Accepted design #232; implementation complete, hosted delivery pending.
Recursive follow-up to the
[production composition plan](production-function-composition-plan.md).
Source-read baseline: `a02eb9f`, after record delivery #230.

Scope is `DumpTable` and three file-local children in
`src/diagnostics/dump_file.cc`, characterization in the existing dump tests,
and this document. No header, iterator/parser/table implementation, native
output, CLI, cache, filter, coverage exclusion or benchmark change.
Header/internal-key assemblers and other unchanged definitions remain
separate recursive frontiers.

## Ownership and evaluation boundaries

Keep size query, random-file acquisition, comparator construction and
Table::Open in their original order and parent scope. Table borrows its
comparator; moving that comparator into an opening helper would leave a dangling
reference. Do not introduce an owner envelope or move construction earlier.
Keep Arbitrary block keys, absent filter/cache and fill_cache=false.
Header success still precedes iterator construction and every data traversal.

The parent owns the table, comparator, read options and iterator through the
borrowed traversal call. The traversal child owns the original first-error
optional, progress Status and each loop iteration's const parsed Result,
default-constructed line and const appended Status.
**Keep those iteration locals alive across the original Next call.**
Moving parse/report/append wholesale into a per-entry helper would destroy
the parsed Error/borrowed view and statuses earlier than before.

A formatting selector borrows the parsed result and valid iterator. Its bad
branch calls existing FormatBadTableEntry(key(), value()); its good branch calls
existing FormatTableEntry(*parsed, value()). Preserve conditional getter calls;
do not pre-fetch an unused encoded key for the good branch. The loop keeps
default string construction followed by assignment from that result, then
remembers malformed-key corruption before output as originally implemented.
Output failure wins immediately. Only successful append permits Next.

Progress failure is interpreted after the loop: non-Corruption wins immediately
over remembered bad keys; Corruption is remembered only if first is absent.
EOF returns that first error or success through existing Finish.
A terminal child borrows mutable progress/first state. Its fatal branch
explicitly moves the borrowed Status (`return std::move(progress)`), rather
than accidentally copying a reference parameter. It introduces no owned
temporary result container; callers directly return its result.

Original const-Status header/append failure copies stay at their logical
locations. Preserve string/error lifetime through Next and returned-status
construction; no catches, retries or changed resource cleanup policy.
Physical move/elision/allocation/destructor behavior remains inspection-based
where no supported observer exists.

## Four-definition recursive ledger

| Definition | Responsibility and children/stopping reason |
|---|---|
| `DumpTable` | Compose existing physical acquisition/configuration/header primitives and parent iterator ownership before borrowed traversal. |
| `DumpTableEntries` | Compose Seek/validity/parse, named formatting, original malformed-key retention, append and Next; delegate terminal policy. |
| `FormatVisitedTableEntry` | Select the existing bad-key or valid-key formatter without changing getter/borrowed-data lifetime. |
| `FinishDiagnosticTable` | Classify terminal progress, preserve fatal moved result or first-corruption retention, then call existing Finish. |

No wrappers merely rename SeekToFirst, Next, ParseInternalKey, key/value,
AppendDiagnosticLine, RememberFirst or Finish. The original loop remains a
composition of these named children and its two new policy/format children,
not an algorithm exempt from recursive review.

## Characterization before extraction

Retain existing bad/valid entry golden vectors, every output failure prefix,
all compression modes, arbitrary comparator order, deletion values, unreachable
filter damage, damaged data reads, first-error retention and late I/O priority.

Add two public characterization cases using existing table assembly and
MemoryFileSystem/StringOutput, not new production seams:

1. An empty-index table has successful header-only EOF. A referenced empty data
   block instead returns Corruption `"table data block is empty"`
   after its header; keep that distinct. Header failure on that referenced-block
   fixture stops before its data read. Check the exact existing acquisition/read
   prefix, not just output emptiness. Constructor suppression is also inspected.
2. Across valid-first/bad-key-first and second-block Corruption/I/O failures,
   fix the exact visited output prefix and terminal error choice. The
   bad-key-first Corruption case retains its earlier error; valid-first
   Corruption returns the new one; I/O wins in both. No second entry is emitted.

Do not instrument unsupported allocator/iterator internals or forge spans.
Borrowed view/error lifetime across Next is verified by source inspection, not
claimed from output equality.

Use the existing anchored coupled native Windows selector:
`^(DumpFileTest|DumpCommandTest|NativeCommandTest|NativeDumpCommandTest|WindowsOutputTest|WalReaderTest|WalWriterTest|WalIoNativeTest|TableTest|TableNativeTest)\.`
Measure original unique classname/name identities after new cases on unchanged
production, compare exactly with final XML, require every addition and no
failure/error/skip. Counts belong to that actual platform/selection.

## Review and delivery

Independent design review/acceptance precedes characterization. Independently
review original-source cases before extraction.
Use two small source topics: parent-owned traversal boundary plus terminal
classification; conditional visited-entry formatting with retained locals.
Check the diagnostic selection between topics and coupled selection at the end.
One non-draft implementation PR may contain both.

Independent combined review checks actual accepted-baseline diff, comparator/
table/iterator/parsed ownership, original getter/default/assignment/Next order,
logical copy/move sites and error priority before push.
Require all exact reviewed-head hosted checks, unchanged authoritative coverage
policy, normal merge and byte-exact reviewed paths in main.
Docs-only acknowledgements are not runtime evidence or file-wide certification.

## Independent design review

Initial bounded read-only review identified a wrong characterization premise:
an index-referenced empty data block is corruption, not successful EOF.
The plan now distinguishes an empty index (success), referenced empty block
(exact corruption), and header failure that suppresses the referenced data read.
Source inspection confirms the existing EnterBlock rejection; no production
policy change is proposed.
The reviewer otherwise found four meaningful responsibilities and consistent
ownership, conditional getters, retained locals, error priority and explicit
borrowed-status moving. No edits/builds/tests or allocation experiments were
performed by that review.

A narrow independent source-only follow-up accepted the corrected premise.
It checked existing empty-table tests, assembly helpers, index validation and
EnterBlock, and confirmed that the referenced-block header-failure fixture
meaningfully suppresses a data read. No further issue, edits/builds/tests or
whole-proposal re-review were reported.

Original-production characterization subsequently corrected the exact empty-data
error text: `ReadDataBlock` rejects an empty decoded block as
`"table data block is empty"` before `EnterBlock` can reach its
`"table data block has no reachable entry"` fallback. The initial native run
failed only that literal assertion; no production change was made. The earlier
reviews established corruption rather than successful EOF but missed this
earlier rejection. Retain both the finding and this correction as review limits.

## Implementation and native evidence

Characterization commit `2c64fd4` precedes production changes. It generalizes
the existing two-block assembler with a borrowed first key, retaining the three
old callers' exact `"bad"` bytes. A local owned valid InternalKey supplies the
new valid-first fixture. Literal outputs and errors do not call production
formatters. Independent bounded characterization review found no significant
issues; it did not edit, build or run tests.

Two public additions are
`EmptyIndexAndReferencedEmptyBlockKeepHeaderReadBoundary` and
`TableTerminalFailuresPreserveExactVisitedPrefixAndPriority`.
The referenced fixture fixes the four-operation acquisition prefix at header
failure and the fifth operation as the first data read. Its terminal matrix
covers all four documented first-key/second-block failure combinations.
The corrected original-source coupled Windows native run has **108 unique
cases**, no failure/error/skip. DumpFile has 26, DumpCommand 6, NativeCommand 3,
NativeDumpCommand 2, WindowsOutput 3, WalReader 17, WalWriter 10, WalIoNative 1,
Table 38 and TableNative 2. The preceding record batch's 106 used these same ten
classes; the two new public cases account for the difference.

Commit `529ac89` extracts parent-owned traversal and terminal classification.
Its diagnostic-only intermediate selection has **40 unique cases**, no
failure/error/skip, all members of the original coupled set.
Commit `91c6659` extracts conditional visited-entry formatting.
The final coupled native run has the **same 108 exact classname/name identities**
as the corrected original-source run and no failure/error/skip.
Artifacts are `diagnostic-table-before.xml`, `diagnostic-table-boundary.xml`
and `diagnostic-table-after.xml` with corresponding build/CTest logs in the
session evidence directory.

Source inspection preserves comparator construction after file acquisition,
table/options/iterator ownership, header-before-iterator ordering, conditional
getters and default line construction followed by assignment. Parsed Result,
line and const appended Status remain in the loop across Next. Existing
header/append const-Status failures retain their logical copy sites; terminal
fatal progress moves explicitly from a borrowed reference. Allocation,
physical copy/elision and destructor equivalence remain inspection-based,
not claimed from XML, output or memory-I/O counts. These cases do not certify
the rest of this file, whole-file validity or the recursive production frontier.

Independent bounded combined review of source, characterization and this record
against accepted-design baseline `a5a0880` found no significant issues. It made
no edits and ran no builds/tests. Hosted checks and actual Windows Debug test-name
parity still gate normal delivery; they are not claimed by this local review.
