# Diagnostic WAL-Record Orchestration Composition

## Status and scope

Design accepted in #229. This is a recursive follow-up to the
[production composition plan](production-function-composition-plan.md).
Source-read baseline: `e3db807`, after byte-escape delivery #228.

Scope is `DumpWalFramed`, `FormatWriteBatch`, `FormatVersionEdit` and six
file-local children in `src/diagnostics/dump_file.cc`, characterization in the
existing dump tests and this document. No public/private header, decoder, WAL
reader, table/native output, CLI, coverage exclusion or benchmark change.
Table traversal and other header/internal-key assemblers remain separate
recursive frontiers. Completing this slice does not certify the source file.

## Preserved protocol and ownership

Open the sequential file before writing its header. Header failure returns
without constructing a reader or reading. Construct the same parent-owned
WalReader only after header success, and delegate the loop by reference.
The existing formatter parameter is borrowed and invoked as an lvalue;
do not copy/move it or introduce a new callback framework.

The loop owns its first remembered corruption and each current WalReadResult.
A fatal reader error wins immediately; EOF returns the remembered first error.
Physical corruption is rendered and appended before remembering it, then
reading continues. A logical record's formatter finishes before the next read,
so its borrowed bytes never outlive the current reader buffer/scratch.
Reader/file lifetime covers all children and first-error destruction.

Malformed WAL batches and MANIFEST edits share exactly the same wrap/report/
remember protocol, parameterized only by their existing type label. Construct
the owned contextual Error, report it, and remember it only after output
succeeds. Successful reporting returns success so later records still dump;
output failure wins over any remembered input corruption. Do not change error
text, offsets, dropped counts, report order or the EOF/read-error distinction.

Keep every original const-Status failed return at its logical reporting,
header, entry or formatter site. New checked propagation moves errors without
additional copies. Preserve contextual Error/line lifetime through reporting,
first-error copying and return-result construction. Status errors and thrown
exceptions remain distinct; no catches or success-shaped fallbacks are added.

For a valid batch, decode once, build/write its record header before calling
Next, then render/write entries in existing order. Header construction keeps
the original string expression; do not switch to a different growth/append
scheme. The parent batch reader remains alive through entry traversal.
Existing FormatWriteBatchEntry and AppendDiagnosticLine are reused.
Valid MANIFEST field output still delegates to AppendVersionEditFields.

## Nine-definition recursive ledger

| Definition | Responsibility and children/stopping reason |
|---|---|
| `DumpWalFramed` | Compose acquisition, existing header emission and parent reader construction before borrowed record traversal. |
| `DumpWalRecords` | Compose read/error/EOF control, physical reporting and existing logical formatter dispatch, owning first-error state. |
| `FormatPhysicalCorruption` | Construct one physical-corruption line through existing scalar/string/error-escape primitives; its three identifying fields are one representation. |
| `ReportPhysicalCorruption` | Compose line formatting, append and first-error retention, preserving append-failure priority. |
| `ReportInvalidRecord` | Compose existing InvalidRecord, AppendRecordError and RememberFirst for either decoder. |
| `FormatWriteBatch` | Compose validated open, shared invalid reporting or header emission plus borrowed entry traversal. |
| `FormatWriteBatchHeader` | Construct one offset/sequence/count identification line using the original expression and existing primitives. |
| `AppendWriteBatchEntries` | Compose trusted Next, existing entry formatter and line append with original failed-return copies. |
| `FormatVersionEdit` | Compose decode, shared invalid reporting, original header and existing field emission. |

Report children own the original contextual error/line/status locals; parents
retain decoded results and reader/batch owners. The new record-loop child
owns the original first-error/event locals. Do not add an event-result enum,
owner type, parameter bag or generic traversal layer.
Existing Finish/RememberFirst/InvalidRecord/AppendRecordError and their
unchanged children remain subject to the final every-definition audit.

## Original characterization

Existing cases already cover malformed logical records, physical corruption,
crash-truncated EOF, output failure at all successful sections and on corruption
reports, initial read failures, empty batches and canonical MANIFEST fields.
Retain these rather than duplicate their individual conditions.

Strengthen the interactions through public DumpFile and existing test-local
StringOutput/MemoryFileSystem:

1. For both log and MANIFEST, a damaged full physical block followed by a
   malformed logical record and a valid record reports in that order, continues
   to the valid payload and returns the exact first physical error. Check
   offsets/type labels and append boundaries rather than only a success flag.
2. On that mixed stream, fail each output append and check the exact successful
   prefix and output-error priority, including failures after corruption was
   already remembered. Header failure must cause no read; do not infer that
   an entry failure forbids the reader's existing block read-ahead.
3. Fail the second MemoryFileSystem sequential read after a
   reported damaged full block. The later I/O error wins, the earlier report
   remains in output, and no next logical record is emitted.

No new production seam, forged payload span or allocator hook.
Allocation throws, precise copies, owner/destructor and mutation interleaving
remain inspection-based where no supported direct observer exists.

Use the anchored native Windows coupled selector from byte-escape delivery:
`^(DumpFileTest|DumpCommandTest|NativeCommandTest|NativeDumpCommandTest|WindowsOutputTest|WalReaderTest|WalWriterTest|WalIoNativeTest|TableTest|TableNativeTest)\.`
Record the actual unique classname/name set after characterization on unchanged
production; require exact final equality, all additions and no failure/error/
skip. Counts are specific to the selected suites/platform, not an older batch.

## Topics and delivery

Independent design review and acceptance precede characterization/source edits.
Independently review original-source characterization before extraction.
Use three small source topics: framed-loop/physical-report composition; shared
invalid reporting at both decoder call sites; batch header/entry composition.
Run the diagnostic selection between topics, then the coupled selection.
One non-draft implementation PR may contain these related commits.

Independent combined review checks actual baseline diff, borrowed lvalue
formatter/record lifetime, copy sites and report/error priority before push.
Require all exact reviewed-head hosted checks including unchanged authoritative
coverage policy, normal merge and byte-exact reviewed-path verification in main.
Docs-only acknowledgements are not runtime evidence.

## Independent design review

Bounded read-only review found no significant issue against unchanged
`e3db807`, the current dump/reader implementations, existing test coverage and
MemoryFileSystem hooks. It confirmed nine substantive responsibilities,
borrowed lvalue/record lifetimes, report-before-remember order, logical
const-Status copy sites and feasible mixed-corruption/second-read cases.
Implementation copy/allocation/destruction ordering still requires its own
review. The reviewer performed no edits/builds/tests or nested delegation.

## Original-production characterization

Three new public cases cover both log/MANIFEST mixed physical-corruption,
malformed-logical and valid-record streams; all five output failure positions
for each stream; and second-read I/O failure after a reported corrupt block.
Explicit golden append vectors fix report boundaries, offsets/type labels,
valid payload and the exact first physical error. Failure cases fix each
successful prefix and fatal output/read error priority.

Header-output failure has only the existing sequential open operation and no
read. No claim is made that later output failure prevents block read-ahead.
Hooks are cleared before captured counters leave scope. Constructor/destructor,
allocation interleaving and precise copy observations remain inspection-based.

Diagnostic source/header remain byte-identical to `2be58f9` during the run.
Native Windows Debug's coupled JUnit contains 106 unique classname/name pairs,
all three additions, with no failures/errors/skips. DumpFile now has 24 cases;
the other selected suite counts remain those of the preceding 103-case byte
batch. This is not the unrelated earlier metadata 106-case selection.

Independent pre-extraction characterization review found no actionable issue
in either stream fixture or the three cases and independently checked the
106-case XML. It confirmed explicit goldens, safe hook/counter scope and honest
read-ahead/copy/allocation limits. No edits/builds/tests were performed.

## Implementation and native parity

Three separate source topics implement the nine-definition ledger after the
characterization commit: framed traversal/physical reporting, shared invalid
reporting and batch header/entry composition.
The parent still constructs/owns its reader after header success and lends
the existing formatter as an lvalue. Original const-Status failed returns stay
at their logical child locations; new physical-report propagation moves errors.
Contextual errors/lines and parent decoded results cover report/remember/return,
while the batch owner covers trusted entry traversal.

Both framed and shared-invalid intermediate selections pass 38 unique cases.
Final native Windows Debug coupled XML has exactly the original 106 unique
classname/name identities, all three additions and no failures/errors/skips.
No header/seam/decoder/table/native/policy/coverage-exclusion changes.
Precise copies, allocation/destruction ordering remain inspection-based.
Local formatting/documentation/whitespace checks and independent combined
review precede publication; reviewed exact-head hosted CI remains a merge gate.

Independent combined read-only review found no significant issue in the
three-path diff, nine-definition ledger and relevant caller/decoder/reader
contracts against `2be58f9`. It independently verified the 106-case identity
parity and both 38-case intermediates. Allocation/copy/destruction equivalence
remains inspection-based rather than allocator-observer evidence.
No edits/builds/tests or hosted-CI verification were performed by the reviewer.
