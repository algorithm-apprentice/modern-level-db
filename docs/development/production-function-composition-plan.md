# Production Function Composition Plan

## Status and scope

Design accepted in #173. Implementation status is recorded per slice below;
accepting this plan does not complete the production-wide goal.
The source baseline is `d508ac144b4115c701ed13ed1bda329fd5aa0755`.
This extends the finite inventory delivered under
[ADR-0077](../adr/0077-bounded-orchestration-composition.md), rather than claiming
that its three completed slices established the style of the whole codebase.

The current scope is the 55 tracked `.cc` files under `src` and
`tools/modern_leveldb_tool.cc`: 56 files and 10,482 source lines. Benchmarks,
tests, fuzz targets, Python tools, generated code, and external dependencies are
not refactoring candidates. Directly dependent headers and characterization
tests may change when implementing a production slice.

A syntax-only tree-sitter scan inventories 741 `function_definition` nodes and
40 lambda expressions, including inline members, constructors, destructors,
templates, and both native implementations. There were no parser error or
missing nodes. These are syntax counts, not a compiler-resolved call graph or a
claim that 741 runtime bodies have passed an atomic-responsibility audit.
The complete signatures, source ranges, immediate statements, calls, and lambda
ranges are retained in session scan evidence. The file dispositions below and
the source-read candidates are the starting worklist, not a final style
certification.

A current-source expanded scan at
`de1ed34ad8c336f3cddc9efb21f7f0ce560f506c` includes tracked production
headers as well as implementation files: 120 files, 17,024 lines,
1,437 tree-sitter `function_definition` nodes and 38 lambdas. It includes
deleted/defaulted special members and reports grammar-recovery nodes in several
declaration-heavy headers, so it is neither directly comparable to the original
741-definition implementation-file inventory nor a trustworthy completion
counter. It does expose previously omitted private-header frontiers such as the
39-definition arena-backed SkipList. The retained JSON remains the mechanical
index; semantic disposition still requires source review and named evidence.

## Recursive design rule

Every function does one named job. When that job consists of independently
meaningful smaller jobs, express them as calls to named functions and apply the
same rule recursively. A composite function coordinates its children; a leaf
implements one indivisible domain operation.

"Atomic" describes responsibility, not concurrency or a single C++ statement.
For example, encoding one fixed-width integer may need several shifts and stores
but still be a leaf. A sequence of option validation, file acquisition, replay,
and installation is not a leaf, even if written briefly. No line threshold,
short-function exemption, or blanket "complex algorithm" exemption applies.
Algorithm invariants constrain the decomposition; they do not exempt the
algorithm from having its independently meaningful steps named.

Review both ordinary functions and lambdas. Standard-library operations and
existing project primitives can already be children; do not add a project
wrapper merely to rename a single call. Conversely, a short function that mixes
two distinct policies still needs decomposition.

Each implementation slice records the parent, children, and stopping reason
for every new or changed leaf. Check the children themselves before declaring
the parent complete. A leaf's stopping reason must identify the domain operation
and its coupled state or representation, not merely say "small" or "complicated."

## Shared operations

Search for prior art before extracting a new helper. Prefer the narrowest scope
that covers its real callers: a private method or file-local function first,
then the owning subsystem's internal interface when sharing crosses files.
Avoid new public API, generic callback frameworks, utility dumping grounds, and
parameter bags that obscure ownership.

Concrete opportunities and limits:

| Operation | Existing evidence | Proposed treatment |
|---|---|---|
| File ordering | `FileBefore` in `src/metadata/version.cc:26`; equivalent checked-build comparator at lines 80-83 | Reuse the existing ordering rule in checked and trusted build paths. Do not merge their validation contracts. |
| Regular Windows file acquisition | `OpenWriter:513-535`, `OpenSequential:629-645`, `OpenRandomAccess:647-662`, `LockFile:886-900` | Share native-path conversion, configured handle opening, and regular-file validation. Keep access, sharing, disposition, flags, contention handling, and caller ownership explicit. |
| Table physical decoding | `ReadExactly`, `ReadStoredBlock`, `ReadBlock`, `IndexHandle`, and `ValidateIndex` already exist in `src/table/table.cc` | Reuse them below footer, index, filter, and lookup stages; do not introduce another read/decode stack. |
| Overlap visitation | `ForEachOverlapping` in `src/engine/lookup.cc:81-137` is already shared by lookup and sampling | Extract candidate selection and the repeated visitor-result handling within this design; preserve both supported visitor result categories and early-stop semantics. |
| First-error retention | `RememberFirstError` is already shared by native writers; `TableBuilder::Record` returns a boolean and dump's `RememberFirst` accepts a borrowed error | Do not force these through a status-returning helper that adds error copies or changes allocation/exception behavior. Share only an exactly compatible primitive if a later slice demonstrates the benefit. |
| New-WAL durability | Recovery syncs the WAL and then the directory at `src/engine/recovery.cc:160-169`; rotation only syncs the directory at `src/engine/database_write.cc:67-74` | Not one interchangeable operation. Keep the policies distinct; do not add a common helper that silently inserts or removes a sync. |
| Native mapping | POSIX inspects size before acquiring a slot and falls back on `fstat`/`mmap` failure; Windows acquires first and reports size/map/close failures | Share only proven platform-neutral predicates if useful. Do not unify the resource transactions or their error policies. |

## File-by-file disposition

`Structured` means the syntax/statement inventory exposes existing primitives
or named compositions and no higher-priority extraction was selected here. It
does **not** waive recursive leaf review. `Decompose` names a concrete frontier
for the task table. Every file, including structured files, participates in the
final per-function audit.

| Production file | Definitions | Disposition and review frontier |
|---|---:|---|
| `src/api/database.cc` | 18 | Decompose public open-option preparation; preserve existing shared read preparation and owner handling. |
| `src/api/iterator.cc` | 12 | Structured: moved-from guards, positioning delegation, and state ownership. |
| `src/api/snapshot.cc` | 3 | Structured: registration ownership and special members. |
| `src/api/write_batch.cc` | 11 | Structured: public batch ownership and mutation delegation. |
| `src/base/coding.cc` | 26 | Structured: bounded scalar and length-prefixed encoding primitives; inspect representation-bound loops as leaves. |
| `src/base/comparator.cc` | 5 | Structured: comparison, separator, and successor primitives; inspect prefix-search/update boundary. |
| `src/base/crc32c.cc` | 4 | Structured: CRC extension and masking operations. |
| `src/base/hash.cc` | 1 | Structured: one hash transform; inspect word/tail stages rather than exempting its algorithm. |
| `src/base/result_internal.cc` | 1 | Structured: first-error retention primitive, already reused. |
| `src/cache/sharded_lru_cache_core.cc` | 38 | Decompose insertion eligibility/admission from preparation, retirement, and pin transfer. |
| `src/diagnostics/dump_command.cc` | 15 | Decompose argument classification from file-loop execution. |
| `src/diagnostics/dump_file.cc` | 17 | Decompose record/entry rendering from decoding, traversal, and output-error handling. |
| `src/diagnostics/posix_output.cc` | 7 | Structured: borrowed output, partial-write progress, and pipe-error handling. |
| `src/diagnostics/windows_output.cc` | 9 | Structured: borrowed handle checks and bounded write progress. |
| `src/engine/build_table.cc` | 2 | Decompose verified-output metadata construction from build orchestration; reuse `WriteTable`. |
| `src/engine/compaction_picker.cc` | 11 | Decompose the input-expansion decision inside `SetupInputs`. |
| `src/engine/compaction.cc` | 11 | Decompose output lifecycle, history/drop policy, overlap advancement, and durable edit assembly. |
| `src/engine/database_background.cc` | 15 | Decompose trivial-move installation and rewrite setup; retain shared edit installation and completed cleanup composition. |
| `src/engine/database_read.cc` | 13 | Structured: capture/read/charge lifecycle; audit lock and exception boundaries before moving any child. |
| `src/engine/database_write.cc` | 5 | Decompose old-log close handling from rotation orchestration; do not merge recovery's sync policy. |
| `src/engine/database.cc` | 10 | Structured: options, opening, recovery installation, and lifetime setup already have named boundaries. |
| `src/engine/db_iterator.cc` | 14 | Decompose direction repositioning and per-entry visibility/history transitions. |
| `src/engine/flush.cc` | 5 | Structured: build, namespace durability, level choice, and edit construction; audit helper leaves. |
| `src/engine/iterators.cc` | 41 | Decompose merge direction changes and child positioning; preserve existing level-source assembly. |
| `src/engine/lookup.cc` | 6 | Decompose L0 candidate selection, visitor handling, and memory-source decisions. |
| `src/engine/recovery.cc` | 21 | Decompose directory inventory, selected-log replay, and recovery publication around existing helpers. |
| `src/engine/seek_statistics.cc` | 6 | Structured: seek charging, retained-file selection, and sampling periods. |
| `src/engine/snapshot_registry.cc` | 5 | Structured: registration, release, and oldest-sequence queries. |
| `src/engine/table_cache.cc` | 4 | Structured: cache lookup and owned table loading; audit admission/error ownership. |
| `src/engine/write_buffer_state.cc` | 7 | Structured: explicit installation and immutable ownership transitions. |
| `src/engine/write_path.cc` | 9 | Structured: queue grouping, commit, and completion; inspect eligibility and append as separate meaningful children. |
| `src/format/internal_key.cc` | 24 | Structured: checked/trusted decoding, lookup encoding, and comparison; inspect separator preparation versus acceptance. |
| `src/format/wal_format.cc` | 8 | Decompose header/payload bounds and checksum validation without changing recovery actions. |
| `src/format/write_batch.cc` | 15 | Decompose one encoded-entry validation from whole-batch validation. |
| `src/memory/arena.cc` | 5 | Structured: alignment, fallback, and block allocation primitives. |
| `src/memory/memtable.cc` | 16 | Structured: entry encoding/decoding, lower-bound lookup, and mutation; keep borrowed values bounded by owner lifetime. |
| `src/metadata/filenames.cc` | 16 | Structured: generation, numeric/name parsing, and CURRENT validation. |
| `src/metadata/version_edit.cc` | 18 | Decompose compound tag payloads while reusing existing field readers. |
| `src/metadata/version_set.cc` | 24 | Decompose MANIFEST acquisition/replay/finalization, unlocked persistence, and new-MANIFEST creation. |
| `src/metadata/version.cc` | 8 | Decompose per-level edit preparation and sorted merge; reuse file ordering. |
| `src/platform/clock.cc` | 2 | Structured: clock query and cancellable sleep. |
| `src/platform/file_system.cc` | 1 | Structured: platform factory and explicit namespace-durability admission. |
| `src/platform/path.cc` | 1 | Structured: path-to-UTF-8 conversion. |
| `src/platform/posix_file_system.cc` | 74 | Decompose mapping eligibility and directory reading; inspect writer/open/lock helper leaves with native error rules intact. |
| `src/platform/serial_executor.cc` | 4 | Structured: scheduling, worker execution, and stop/join lifetime boundaries. |
| `src/platform/windows_file_system.cc` | 61 | Decompose path validation/resolution, database volume admission, shared regular-file opening, mapping, enumeration, and failed read completion. |
| `src/table/block_builder.cc` | 5 | Structured: prefix-compressed entry construction and restart management; audit common-prefix computation separately. |
| `src/table/block_format.cc` | 12 | Structured: block handles, footer/trailer, bounds, checksum, and compression dispatch. |
| `src/table/block.cc` | 26 | Decompose restart search, forward positioning, and full-entry validation stages. |
| `src/table/bloom_filter.cc` | 6 | Structured: sizing, construction, and probing; inspect hashing versus bit-probe loops as leaf boundaries. |
| `src/table/compression.cc` | 5 | Decompose codec-specific compression/decompression from selection and worthwhile-size policy. |
| `src/table/filter_block.cc` | 10 | Structured: filter grouping, offset layout, creation, and membership; audit offset validation versus layout decoding. |
| `src/table/table_builder.cc` | 12 | Decompose metaindex/filter completion, index completion, and footer persistence. |
| `src/table/table.cc` | 22 | Decompose footer, validated index, filter-handle discovery, and data-block lookup. |
| `src/wal/wal_io.cc` | 12 | Decompose physical failure recovery and logical fragment transitions; include fragment-writing lambda/loop responsibilities. |
| `tools/modern_leveldb_tool.cc` | 2 | Structured: native/portable entrypoints delegate to the same command implementation. |

## Initial small-task worklist

Names below describe intended responsibilities, not frozen signatures. Each row
is a bounded implementation/characterization slice. The list is deliberately
not limited to the previously delivered three candidates. New child tasks found
by recursive inspection must be added, not hidden behind completion of a row.
Task counts are not mandatory PR counts; related slices may share an accepted
design, while implementation stays responsibility-sized.

All locations refer to the baseline above. Dependencies are actual code/design
dependencies; priority waves are scheduling suggestions, not extra dependencies.

| ID | Baseline parent | Child responsibility and recursive frontier | Prerequisites |
|---|---|---|---|
| D01 | `dump_file.cc:127-166`, `245-309` | Two independent implementation slices: D01a renders a batch entry; D01b renders a table entry. Separate each from reader/traversal loops and reuse compatible byte escaping. | None |
| D02 | `dump_file.cc:177-243` | Render comparator/scalar fields and each file/pointer record through named payload formatters; preserve record order. | None |
| D03 | `dump_command.cc:69-104` | Classify help/invalid/dump arguments before executing the existing file loop; separate file-loop error policy from syntax validation. | None |
| M01 | `version_set.cc:112-135` | Acquire the CURRENT-selected MANIFEST: read/parse CURRENT, validate its number, open the file with the existing NotFound translation. | None |
| M02 | `version_set.cc:137-191` | Apply one MANIFEST record: decode, check comparator, checked builder application, compact-pointer updates, and counter accumulation as named children. | None |
| M03 | `version_set.cc:193-226` | Validate recovered counters, build the checked version, and publish the recovered state as distinct children. | M02 |
| M04 | `version_set.cc:288-334` | Name the unlocked write/relock boundary; keep all candidate, snapshot, and install-reservation allocation before I/O. | None |
| M05 | `version_set.cc:364-403` | Create and snapshot a new MANIFEST separately from append/sync/CURRENT installation. | None |
| M06 | `version_edit.cc:194-270` | Decode compact-pointer, deleted-file, and new-file payloads using existing `ReadLevel`, `ReadFileNumber`, and `ReadInternalKey`. | None |
| M07 | `version.cc:96-126` | Prepare per-level deletions/additions with named counting, reservation, and metadata initialization boundaries. | None |
| M08 | `version.cc:128-156`, `72-94` | Merge one level's sorted files; preserve deletion filtering and linear merge, and share `FileBefore` with checked sorting. | None |
| R01 | `recovery.cc:114-150` | Inventory relevant tables/logs separately from referenced-table validation, sorting, and number reservation. | None |
| R02 | `recovery.cc:338-374` | Replay selected logs and finish replay separately from new-WAL creation and recovered-state installation; retain `CreateRecoveryLog`. | None |
| Q01 | `lookup.cc:81-137` | Select and order L0 candidates separately from deeper-level selection and visitation. | None |
| Q02 | `lookup.cc:81-137` | Share visitor error/continue/stop handling without erasing its return-category contract or changing sampling. | None |
| Q03 | `lookup.cc:141-194` | Decide the memory sources using existing Missing/Value/Deletion semantics; compose memory and table search at the caller. | None |
| T01 | `table.cc:164-191` | Read/decode a footer and load/validate an index through existing physical-read primitives. | None |
| T02 | `table.cc:122-160` | Find/decode an optional filter handle separately from loading and owning filter contents. | None |
| T03 | `table.cc:231-273` | Search a loaded data block separately from index/filter routing; copy values before releasing the block pin. | None |
| T04 | `table_builder.cc:167-185` | Finish filter/metaindex and pending index entries as named children, preserving sticky error gates and buffers. | None |
| T05 | `table_builder.cc:186-198` | Encode/append the footer and persist the finished table as named children; retain Finish's close/error priority. | None |
| T06 | `build_table.cc:41-80` | Verify the completed table and construct its metadata separately from source iteration/file creation and existing `WriteTable`. | None |
| C01 | `compaction.cc:115-135` | Open an output, initialize its key range, append an entry, and decide its size boundary through named children. | None |
| C02 | `compaction.cc:138-157` | Finish/reset a builder, verify its reopened table, and record output metadata through named children. | None |
| C03 | `compaction.cc:188-262` | Maintain user-key history and decide entry retention through named children; keep per-entry orchestration and final output/edit assembly separate. | None |
| C04 | `compaction.cc:37-52`, `72-87` | Advance grandparent overlap and test one deeper level's coverage as named state operations, then compose stop/drop decisions. | None |
| C05 | `compaction_picker.cc:127-170` | Attempt bounded input expansion separately from initial two-level closure and grandparent collection. | None |
| I01 | `iterators.cc:268-327` | Name forward/reverse direction transitions; each transition composes per-child repositioning, then Next/Prev advance and select. | None |
| I02 | `db_iterator.cc:115-209` | Name direction-change positioning and per-entry visible-history transitions without separating borrowed key/value from their owners. | None |
| I03 | `block.cc:208-262` | Find a restart, restore/reuse a checkpoint, and scan to the lower bound through distinct children; preserve optimized current-position reuse. | None |
| I04 | `block.cc:116-159`, `336-373` | Name entry decoding, restart validation, reconstruction, and visitation/iterator-state commit; reuse the existing `DecodeEntry` primitive. | None |
| F01 | `format/write_batch.cc:21-61` | Validate one encoded record; compose record-count iteration and trailing-byte checks without altering trusted Next. | None |
| F02 | `format/wal_format.cc:120-171` | Name header/payload bounds and checksum validation; retain zero-marker and unknown-type check ordering/recovery actions. | None |
| F03 | `table/compression.cc:47-137` | Name codec-specific adapters; compose selection and worthwhile-size policy using existing `PrepareOutput`. | None |
| W01 | `wal_io.cc:157-210` | Decode one physical fragment and apply its discard/truncated-tail recovery policy separately from block filling. | None |
| W02 | `wal_io.cc:83-155` | Name full/first/continuation/zero transitions and partial-record interruption handling; keep event/continue control explicit. | None |
| N01 | `windows_file_system.cc:100-151` | Validate ordinary path components separately from accepted namespace forms and absolute-path resolution. | None |
| N02 | `windows_file_system.cc:100-151` | Resolve an absolute path with the current grow/retry loop, then encode the extended drive/UNC form as a named child. | None |
| N03 | `windows_file_system.cc:556-627` | Inspect existing/missing database paths and resolve the admitted directory handle before volume checks. | None |
| N04 | `windows_file_system.cc:556-627` | Validate resolved local fixed NTFS policy through named drive/volume/filesystem checks; preserve explicit weak-durability consent. | None |
| N05 | `windows_file_system.cc:513-535`, `629-662`, `886-912` | Share regular-file opening/validation among writer, sequential reader, random reader, and lock acquisition. | None |
| N06 | `posix_file_system.cc:598-654` | Determine mapped length separately from the slot/map/commit transaction, retaining copied fallback on size-inspection failure. | None |
| N07 | `windows_file_system.cc:647-710` | Name eligibility and section/view acquisition, while retaining the pending mapping guard through close checks and owner allocation. | None |
| N08 | `posix_file_system.cc:695-729`, `windows_file_system.cc:737-782` | Name native directory-entry collection separately from acquisition/close/error precedence, using separate platform-specific children. | None |
| N09 | `windows_file_system.cc:271-318` | Complete or drain/cancel a positioned read as a named operation while event/OVERLAPPED/output owners remain alive. | None |
| A01 | `api/database.cc:96-118` | Prepare validated engine options using existing compression translation/validation and options conversion. | None |
| A02 | `database_background.cc:149-189` | Name trivial-move installation separately from rewrite setup, preserving shared `ApplyBackgroundEdit` and pending outputs. | None |
| A03 | `database_write.cc:56-88` | Name old-WAL close/error handling; compose preparation, namespace durability, close, installation, and scheduling without merging policies. | None |
| K01 | `sharded_lru_cache_core.cc:182-210` | Name admission eligibility and locked insertion/accounting; keep retired-entry destruction outside the shard mutex. | None |

Suggested waves: start with D01-D03, M06, F03, and A01 to establish the leaf
review pattern; then tackle table stages, lookup, and recovery; then MANIFEST
publication, compaction, iterators, native resource transactions, and cache
admission. This is a risk/value order, not permission to leave later waves or
structured files unexamined. Same-file changes are sequential; independent
preparation does not authorize simultaneous writes in this worktree.

M03 uses the private replay-state helper introduced by M02. Other extractions
retain existing caller interfaces and can be prepared without their neighboring
extractions; sharing a file or a suggested wave order is not a prerequisite.
Re-evaluate this table if an accepted slice changes an interface or adds a
concrete helper/type dependency.

### Coherent PR-sized batches

At the owner's subsequent request, group related responsibility-sized slices
into moderately larger PRs to amortize CI. Keep small internal commits and
focused validation, use one shared reviewed design for a coherent batch, and
independently review the complete implementation diff and interactions before
publication. Do not require a PR per helper or mix unrelated subsystems merely
to increase size.

After the already-published D01a PR, the
[offline dump composition design](offline-dump-composition-design.md) groups
D01b, D02, and D03 into the next implementation PR. It reuses D01a's quoted-field
operation and therefore waits for that implementation to merge before its
design publication. Engine/native/benchmark work remains outside that batch.
Do not modify an already-running published head solely to fill the next batch.

## First implementation slice: D01a

Start with only WAL batch-entry rendering in `FormatWriteBatch`. Table-entry
rendering, MANIFEST rendering, traversal, byte-escape implementation, command
handling, and all other worklist entries remain separate work.

The intended file-local composition is:

- `FormatWriteBatch` owns checked batch decoding, the record header, entry
  iteration, one diagnostic append per entry, and immediate output-failure
  propagation. It calls `FormatWriteBatchEntry` instead of constructing entry
  text in its loop.
- `FormatWriteBatchEntry` composes `WriteBatchEntryPrefix` and
  `AppendDiagnosticBytesField`, including the value field only for a put.
- `WriteBatchEntryPrefix` renders the existing put/delete and decimal sequence
  prefix. Its leaf responsibility is this fixed prefix grammar, implemented
  using ordinary string operations and decimal conversion.
- `AppendDiagnosticBytesField` appends one named, single-quoted byte field.
  Reuse it for both key and value and delegate escaping to the existing
  `EscapeDiagnosticBytes`. The field grammar is one responsibility; the
  existing shared escaper remains a subsequent recursive-review frontier, not
  a newly certified atomic leaf.

These are private formatting operations, not new public API or an output
abstraction. All borrowed entry bytes remain valid for the complete rendering
call. Build the complete string before `AppendDiagnosticLine`; retain its LF
handling and exactly one output append per line. Preserve field order, put/delete
spelling, decimal sequences, escaping, empty fields, deletion's absent value
field, and the absence of new catches or success-shaped error fallbacks.

Characterization uses the public `DumpFile` operation. Retain
`DumpsWriteBatchesWithEscapedBinaryDataAndPerEntrySequences` and
`PropagatesOutputFailureFromEveryRenderedSection` in
`tests/unit/diagnostics/dump_file_test.cc`. Before extraction, add a case with
empty put key/value and an empty delete key whose two sequences end at
`MaxSequenceNumber`; assert exact text and append boundaries. This is
unchanged-behavior characterization and must pass on the original code, not a
manufactured red test.

Design publication/merge precedes implementation. Run the focused dump and
command cases after extraction, then obtain a separate independent code review
before publishing the implementation PR. D01a's completion does not complete
D01, the dump file's recursive audit, or the production-wide goal.

### D01a implementation record

The entry-rendering extraction is implemented; its associated implementation PR
records delivery status. This slice changes only WAL entry rendering and its
characterization, not table/MANIFEST text, traversal, or escaping.

| Function | Responsibility and stopping boundary |
|---|---|
| `FormatWriteBatch` | Its entry loop now composes checked iteration, complete entry rendering, and diagnostic append/error propagation. The remaining record-level decoding/header/error paths are later recursive-review frontiers. |
| `FormatWriteBatchEntry` | Composition of prefix rendering and quoted key/value fields, with the existing put-only value condition. |
| `WriteBatchEntryPrefix` | One fixed prefix grammar: indentation, trusted put/delete spelling, and decimal sequence. Ordinary string operations and decimal conversion are its representation primitives. |
| `AppendDiagnosticBytesField` | One named quoted-field grammar, shared by key and value; it delegates byte escaping to the existing shared operation. The escaper's internal decomposition remains pending. |

`DumpsEmptyBatchEntryFieldsAtTheSequenceBoundary` was added before extraction and
passed against the original formatter. It checks all four append boundaries,
empty put key/value, delete's absent value field, and the final two valid
sequences. The same 28 focused dump/command cases passed before and after
extraction, including binary escaping and every-rendered-section output
failures. Formatting and the warnings-as-errors Debug build are checked for
the implementation; this evidence is not a claim about unmodified worklist
entries or the whole production tree.

A separate independent implementation persona reviewed the D01a diff and
surrounding reader/formatter contracts and found no actionable findings. This
was static code review; the main agent separately verified the before/after
JUnit case sets and results. Publication and merge still require green checks
on the exact implementation PR head.

### D01b/D02/D03 combined implementation record

The [offline dump batch design](offline-dump-composition-design.md) was accepted
in #175 before implementation. Table entry rendering/shared fields, MANIFEST
payload groups, and command classification/per-file execution are implemented
as separate topic commits for one coherent implementation PR. Its detailed
ledger identifies every new function and the changed parent responsibilities.

The three added characterization boundaries passed on unchanged production
code. The same 30 focused cases passed after the combined extraction, with
exact JUnit case-set parity and no failures, errors, or skips. Individual
table and MANIFEST slices each passed the 19 dump-file cases.
Independent combined implementation review found no actionable issues and
independently confirmed both JUnit case sets; it did not execute builds/tests.
Remote delivery still requires green exact-head CI.

This completes the selected extraction work locally, not the recursive audit
of either diagnostics file. Byte escaping, structured-key rendering, remaining
record/table orchestration, and all other production worklist frontiers remain
subject to the completion criterion below.

## Representative recursive designs

### M06/M07/M08 implementation record

The [metadata composition design](metadata-composition-design.md) was reviewed
and accepted in #177 independently of the offline dump implementation.
The paired compound-payload encoders/decoders, trusted delta preparation,
shared metadata owner creation, and per-level merge are implemented in small
topic commits for one combined PR. Its design document records every new
function's composition or responsibility-based stopping boundary.

Characterization passed before changing production code, including exact
compound errors, zero-size bytes, mixed-failure order, L0 ties/replacements,
retained ownership and budgets, and a measured comparison-count ceiling.
The same 79 focused cases passed before/after the combined extraction.
Independent implementation review found no actionable issues and separately
confirmed before/after and per-slice JUnit evidence without executing tests.
Exact-head CI remains the delivery gate; all other recursive-review frontiers
remain open.

### T01/T02/T03 implementation record

The [table opening and lookup design](table-open-lookup-composition-design.md)
was independently reviewed and accepted in #179 before implementation.
Footer/validated-index acquisition, optional filter selection/loading, and
pinned block lookup/owned value copy are implemented in separate topic commits.
The detailed ledger covers every new and changed function and its stopping
boundary, without certifying unchanged table internals.

Four added public-operation cases and strengthened existing cases passed on
unchanged production code. The combined extraction preserves all 64 focused
TableTest/TableCacheTest/DumpFileTest cases with exact before/after JUnit parity;
the opening and filter slices each preserve the same 38 TableTest cases.
Independent implementation review found no significant issues and confirmed
the existing before/after evidence without executing tests.
Exact published-head CI remains the delivery gate.

Table writer/finalization, index-validation visitor, cache/iterator internals,
and all other recursive-review frontiers remain open.

### T04/T05/T06 implementation record

The [table writer composition design](table-writer-composition-design.md)
was independently reviewed and accepted in #181 before implementation.
Terminal filter/metaindex/index composition, footer persistence, and completed
table verification/owned metadata construction use separate topic commits for
one implementation PR. Its detailed ledger covers every new/changed function.

New characterization separates stored append prefixes from complete-block
reported size and proves metadata ownership after source/cache release.
All 31 focused cases pass before/after with exact JUnit parity; both builder
slices pass 19 cases. Independent implementation review found no significant
issues and confirmed existing JUnit evidence without executing tests.
Exact published-head CI remains the delivery gate.
Entry/data/raw-block writing, existing WriteTable, and remaining recursive
frontiers are not certified by this batch.

### F03 implementation record

The [block codec composition design](block-compression-composition-design.md)
was reviewed and accepted in #183 before implementation; its extraction was
delivered in #184. Four private native-codec adapters compose existing codec
primitives and storage operations, while public callers retain selection,
scratch admission and common savings policy. Its ledger names all new/changed
functions and remaining frontiers.

Three new characterization cases and strengthened existing cases passed on
unchanged production. All 77 selected codec/stored-block/writer/reader cases
passed before/after with exact JUnit parity; the compression slice passed
14 real/mock codec cases. Independent implementation review found no significant
issues. All 20 applicable exact-head CI checks passed before merge.
This does not certify broader codec/table internals or whole-production
recursive completion.

### F01/F02 implementation record

The [checked record design](record-validation-composition-design.md)
was independently reviewed and accepted in #185 before implementation.
Its implementation was delivered in #186 after all 20 exact-head checks passed.
Per-record/complete batch proof and WAL bounds/checksum composition use separate
topic commits. The detailed ledger names all seven new/changed functions.

Four new cases and strengthened existing errors/recovery tuples passed before
production edits. All 58 selected batch/fragmenter/decoder/reader/writer cases
pass before/after with exact JUnit parity; the batch slice passes 17 cases.
Independent combined implementation review found no significant issues without
executing tests.
Trusted iteration, fragmentation, WAL I/O recovery and remaining recursive
frontiers are not certified by this batch.

### A01 implementation record

The [API opening design](api-open-options-composition-design.md) was reviewed
and accepted in #187; its implementation was delivered in #188 after all
20 exact-head checks passed. PrepareEngineOptions composes restart validation,
compression translation/validation and existing option mapping before Open
acquires its engine and constructs the shared owner.

All 16 selected public API cases passed before/after with exact JUnit parity,
including mixed-invalid priority and native Zstd endpoints. The actual
modern_leveldb_public_api_tests target, separate from the general unit target,
was rebuilt for both recorded runs. Independent implementation review found
no significant issue and inspected the XML without running tests.
Broader engine acquisition, allocation exceptions and remaining recursive
frontiers are not certified by this batch.

### Q01/Q02/Q03 implementation record

The [lookup source design](lookup-source-composition-design.md) was independently
reviewed and accepted in #189; implementation was delivered in #190 after all
20 exact-head checks passed. Separate topic commits compose ordered L0/lazy
deeper candidates, memory decisions and the relocated private shared owned copy.
The detailed ledger includes cursor methods, parent compositions, the new
private header definition and existing lambda frontiers.

Five added public-interface characterization cases passed on original
production before extraction. All 64 selected lookup/sampling/seek/table cases
pass before/after with exact JUnit parity; both intermediate slices pass the
same 18 lookup/sampling cases. Original-slot charge identity, untouched
non-value output, memory ownership after release and lazy stop/error selection
are directly characterized. Independent implementation review found no
significant issue and independently inspected the XML without running tests.
Remaining recursive frontiers are not certified.

### R01/R02 implementation record

The [recovery input/install design](recovery-input-install-composition-design.md)
was independently reviewed and accepted in #191. Implementation was delivered
in #192 after all 20 exact-head checks passed. Owned number inventory,
selected-log/final-flush orchestration and new-WAL checked installation compose
existing recovery primitives without changing caller-owned replay/eviction
guards or lock-last lifetime.

All 46 recovery/version/insertion/commit cases passed before/after with exact
JUnit parity; the inventory slice passed 17 recovery cases. Review's reservation
oracle gap was corrected with selected log 20 and outputs 21/22, recharacterized
against original and final source, and independently confirmed. Deeper
LogReplay/version/WAL/native operations remain recursive frontiers.

### C01/C02/C03/C04 implementation record

The [compaction composition design](compaction-output-history-composition-design.md)
was independently reviewed and accepted in #193. Separate monotonic cursor,
output transaction and visible-history/entry/finalization topic commits were
delivered in #194 after all 20 exact-head checks passed. The
implementation follows its fifteen-definition recursive ledger.

Five added characterization cases passed on original production before
extraction. All 50 compaction/picker/table-builder cases pass before/after with
exact JUnit parity, and intermediate slices pass 18 and 37 cases.
Snapshot/coverage short circuits, mixed error priority, metadata ownership and
input-failure suppression of completion are directly characterized.
Independent implementation review found no significant issue and independently
inspected the XML without executing tests.
Deeper primitives and whole-production recursive completion are not certified.

### A02/A03 implementation record

The [engine transaction design](background-rotation-composition-design.md) was
independently reviewed and accepted in #197. Separate rotation and trivial-move
topic commits compose replacement acquisition, old-close sticky policy, original
installation/scheduling and unchanged-file edit application.

Four existing characterization cases were strengthened before source edits.
All 54 DatabaseTest cases pass before/after with exact JUnit parity, including
reruns on the integrated #194 baseline. Returned-close-error generation
installation, thrown-close noninstallation, preparation retry and trivial
move/no-cleanup distinctions are directly asserted.
Independent combined implementation review found no actionable issue and
independently inspected the XML without running tests. Delivery completed in
#198 after all 20 exact-head checks succeeded; rewrite setup, write-buffer internals and remaining recursive
frontiers stay open.

### C05 implementation record

The [bounded input expansion design](compaction-input-expansion-composition-design.md)
was accepted in #195 and delivered in #196 after independent implementation
review and all 20 exact-head checks succeeded. Original/final 29-case parity
includes exact budget boundary, expanded-next boundary chain, lazy rejection,
original file identities, source compact pointer and final grandparent range.
Integrated picker/compaction regression includes all 34 cases after #194.
Expansion's private owned replacement result leaves range widening after parent
owner replacement; deeper overlap/boundary primitives remain recursive frontiers.

### M01/M02/M03/M04/M05 implementation record

The [MANIFEST transaction design](manifest-transaction-composition-design.md)
was independently reviewed and accepted in #201 before implementation.
Selected-descriptor acquisition, private replay state, ordered record updates,
final checked publication, explicit prepared write/relock and staged snapshot
writer acquisition follow its fourteen-definition responsibility ledger.

Six new original-source characterization cases and the same 115 metadata,
recovery and database cases pass before/after with exact JUnit parity; the
recovery topic passes 61 cases. They preserve multi-fault/error-envelope
priority, sparse counters/pointers, historical maxima, no later-block recovery
read, pre-append thrown failure/retry, candidate ownership and concurrent
allocation/sequence changes. Independent combined review found no significant
issue and directly verified both 115-case artifacts without running tests.
Delivery completed in #203 after all 20 exact-head checks succeeded;
unmodified helper leaves remain open.

### K01 implementation record

The [cache admission design](cache-admission-composition-design.md) was accepted
in #199 and delivered in #200 after independent combined review and all 20
exact-head checks succeeded. Eligibility and locked commit reuse existing cache
primitives while the parent retains scoped locking, pending ownership,
unlocked reclamation and returned pin transfer.
Three added original-source characterizations verify replacement accounting,
new-mapping erasure during replacement reclamation and allocation-free
overflow rejection before prepared bucket growth. Exact 31-case original/final
cache/allocation/table-cache parity uses both real test executables.
Deeper table/list/retention/typed-cache helpers remain recursive frontiers.

### I01/I02 implementation record

The [engine iterator transition design](engine-iterator-transition-composition-design.md)
was independently reviewed and accepted in #205. Separate merge-direction and
user-position/history topic commits follow its fifteen-function ledger.
Four added original-source cases measure exact child ordering/failure stopping,
owned binary/empty reverse outputs over reused buffers, and beginning outcome;
reverse sampling additionally measures exact period counts.
The same 105 iterator/compaction/database cases pass before/after with exact
JUnit parity; merge direction alone passes eight cases. Independent combined
review found no significant issue and directly confirmed both artifacts without
running tests. Delivery completed in #207 after all 20 exact-head checks
succeeded. A subsequent CI-coverage characterization adds the malformed-key
reverse-reposition boundary; the delivered integrated selection contains 106
cases. This does not change the recorded 105-case original/final comparison.
Other source, sampling, bulk positioning and deeper block iterator frontiers
stay open.

### Recent delivery reconciliation

These records reconcile completed module slices, not completed files or a
production-wide atomic-responsibility certification. Their detailed designs
retain definition-level ledgers, original-source characterization and limits.
All delivered implementations below received independent combined review,
passed all 20 exact reviewed-head checks, and were normally merged with reviewed
changed paths verified in main.

| Bounded slice | Detailed design | Delivery and focused evidence |
|---|---|---|
| W01/W02 physical recovery and logical transitions | [WAL reader transitions](wal-reader-transition-composition-design.md) | Delivered in #204; corrected integrated selector covers 73 WalReaderTest/WalWriterTest/WalIoNativeTest cases, not the earlier incomplete 49-case selection. |
| I03/I04 checkpoint positioning and entry validation | [Block positioning](block-positioning-composition-design.md) | Delivered in #208 after accepted #206; 129-case original/final parity, with a 25-case seek intermediate selection. |
| Recursive WAL append/block-fill progress | [WAL progress](wal-block-progress-composition-design.md) | Delivered in #211 after accepted #210; 148-case original/final parity, with an 11-case writer/native intermediate selection. |
| N01/N02/N05 native path and shared regular opening | [Native opening](native-path-opening-composition-design.md) | Delivered in #212 after accepted #209; 121-case original/final parity, with a 46-case path intermediate selection. |
| N03/N04 directory resolution and storage admission | [Native directory admission](native-directory-admission-composition-design.md) | Delivered in #214 after accepted #213; 120-case original/final parity, with a 49-case resolution intermediate selection. |
| N06 and POSIX portion of N08 | [POSIX mapping/directory](posix-mapping-directory-composition-design.md) | Delivered in #216 after accepted #215; actual native CI logs have identical original/final Linux 766 and macOS 761 unique case sets, including focused 82/77-case sets. |
| N07 and Windows portion of N08 | [Windows mapping/enumeration](windows-mapping-enumeration-composition-design.md) | Delivered in #218 after accepted #217; 163-case original/final native parity, 54-case mapping intermediate, and actual hosted Windows Debug selected names match the original 163. |
| N09 and shared positioned-read range proof | [Windows positioned reads](windows-positioned-read-composition-design.md) | Delivered in #221 after accepted #220; 166-case original/final native parity, 57-case preparation intermediate, and actual hosted Windows Debug selected names match the original 166. |
| Recursive CURRENT I/O and runtime validation | [CURRENT/runtime validation](metadata-current-validation-composition-design.md) | Delivered in #223 after accepted #222; 119-case original/final native parity, separate 35-case read/proof intermediates, and actual hosted Windows Debug selected names match the original 119. |
| Recursive snapshot materialization and live-file union | [Snapshot/liveness](metadata-snapshot-liveness-composition-design.md) | Delivered in #225 after accepted #224; 121-case original/final native parity, 37-case snapshot intermediate, and actual hosted Windows Debug selected names match the original 121. |
| Recursive byte selection and fixed-width escaping | [Diagnostic byte escaping](diagnostic-byte-escape-composition-design.md) | Delivered in #228 after accepted #227; 103-case original/final native parity, 35-case diagnostic intermediate, and actual hosted Windows Debug selected names match the original 103. |
| Recursive framed-record traversal and logical reporting | [Diagnostic record orchestration](diagnostic-record-orchestration-composition-design.md) | Delivered in #230 after accepted #229; 106-case original/final native parity, separate 38-case framed/reporting intermediates, and actual hosted Windows Debug selected names match the original 106. |
| Recursive parent-owned table traversal | [Diagnostic table traversal](diagnostic-table-traversal-composition-design.md) | Delivered in #233 after accepted #232; 108-case original/final native parity, 40-case traversal intermediate, and actual hosted Windows Debug selected names match the original 108. |
| Recursive canonical classification and borrowed type dispatch | [Diagnostic file dispatch](diagnostic-file-dispatch-composition-design.md) | Delivered in #236 after accepted #235; 110-case original/final native parity, 42-case classification intermediate, and actual hosted Windows Debug selected names match the original 110. |
| Recursive recovered-counter accumulation and publication | [Recovered metadata counters](metadata-recovered-counter-composition-design.md) | Delivered in #238 after accepted #237; 123-case original/final native parity, 39-case accumulator intermediate, and actual hosted Windows Debug selected names match the original 123. |
| Recursive predecessor checkpoint and reverse suffix scans | [Block reverse positioning](block-reverse-positioning-composition-design.md) | Delivered in #241 after accepted #240; 201-case original/final native parity, 42-case predecessor intermediate, and actual hosted Windows Debug selected names match the original 201. |
| Recursive hash word folding and bounded residual-tail mixing | [Hash word/tail composition](hash-word-tail-composition-design.md) | Delivered in #243 after accepted #242; 96-case original/final native parity, six-case word intermediate, and actual hosted Windows Debug selected names match the original 96 and delivered reverse selection's 201. |
| Recursive byte/trailer comparison, checked shortening and prefix-block stages | [Key comparison/shortening](key-comparison-shortening-composition-design.md) | Delivered in #245 after accepted #244; 238-case original/final native parity, two 35-case foundation intermediates, and actual hosted Windows Debug selected names match the original 238. |
| Recursive required-field proof and parent-owned prepared publication | [Prepared metadata publication](metadata-prepared-publication-composition-design.md) | Delivered in #248 after accepted #247; 125-case original/final native parity, two 41-case VersionSet intermediates, and actual hosted Windows Debug selected names match the original 125. |
| Recursive lookup storage lifecycle and shared trusted key emission | [Lookup storage/encoding](lookup-key-storage-encoding-composition-design.md) | Delivered in #250 after accepted #249; corrected 151-case original/final native parity, 36-case foundation intermediates, and actual hosted Windows Debug selected names match the original 151. |
| Recursive sticky-error capture and parent-owned diagnostic formatting | [Foundation error context](foundation-error-context-composition-design.md) | Delivered in #253 after accepted #252; 166-case original/final native parity, 26-case foundation intermediate, and actual hosted Windows Debug selected names match the original 166. |
| Recursive scalar/read-record codecs, block admission and MemTable lookup | [Read-record codecs](read-record-codec-composition-design.md) | Delivered in #254 from one locally reviewed larger batch; corrected 296-case original/final native parity, exact 99/197/131 topic subsets, and actual hosted Windows Debug selected names match the original 296. |
| Complete cache-core index, pin, retention, accounting and teardown lifecycle | [Cache lifecycle](cache-lifecycle-composition-design.md) | Delivered in #255 from one locally reviewed larger batch; 35-case original/final parity, exact 28/29 topic subsets, complete core/typed-wrapper disposition, and actual hosted Windows Debug selected names match the original 35. |
| Complete private-header SkipList layout, publication and search lifecycle | [SkipList lifecycle](skiplist-lifecycle-composition-design.md) | Delivered in #256 from one locally reviewed larger batch; corrected 100-case original/final parity, exact 32-case publication subset, complete 48-definition disposition, and actual hosted Windows Debug selected names match the original 100. |
| Complete write queue commit and buffer-generation ownership lifecycle | [Write commit lifecycle](write-commit-lifecycle-composition-design.md) | Delivered in #257 from one locally reviewed larger batch; corrected 114-case original/final parity, exact 40/71/74 topic subsets, complete 61-definition/three-lambda disposition, and actual hosted Windows Debug selected names match the original 114. |
| Complete trusted WriteBatch and WAL fragment codec lifecycle | [Record codec lifecycle](record-codec-lifecycle-composition-design.md) | Delivered in #258 from one locally reviewed larger batch; corrected 144-case original/final parity, exact 94/121 topic subsets, complete 59-definition disposition, and actual hosted Windows Debug selected names match the original 144. |
| Complete internal-key, Arena and MemTable ownership lifecycle | [Key/memory lifecycle](key-memory-lifecycle-composition-design.md) | Delivered in #259 from one locally reviewed larger batch; 200-case original/final parity, exact 39/33 topic subsets, complete 115-definition disposition, and actual hosted Windows Debug selected names match the original 200. |
| Complete Block, Iterator, BlockBuilder and block-format representation lifecycle | [Block representation lifecycle](block-representation-lifecycle-composition-design.md) | Delivered in #260 from one locally reviewed larger batch; corrected 208-case original/final parity, exact 121/133 topic subsets, complete 96-definition disposition, and actual hosted Windows Debug selected names match the original 208. |
| Complete Bloom and filter-block generation/query lifecycle | [Table filter lifecycle](table-filter-lifecycle-composition-design.md) | Delivered in #261 from one locally reviewed larger batch; 162-case original/final parity, exact 78/144 topic subsets, complete 34-definition disposition, and actual hosted Windows Debug selected names match the original 162. |

For #216, local Windows builds were not used as POSIX runtime evidence.
The reviewed characterization-only draft first ran unchanged production on
Linux/macOS; production topics were added afterward. Original run 38179010927
and implementation run 38191108865 establish exact native case-set parity.
The draft became ready only after implementation review, native comparison and
all 20 reviewed-head checks succeeded; it is now merged, not a pending draft.
Case counts from native CTest logs are not represented as JUnit artifacts.

#218 adds five mapping/enumeration characterization cases; #221 adds three
positioned-read cases to that native selection, so 163 and 166 are different
batch baselines, not interchangeable counts. Genuine still-pending kernel
cancellation/draining and allocation throws remain inspection-based; the
existing completion-failure seam waits for real completion before reporting
failure. Native Windows evidence is not inferred from Linux coverage or
design-only acknowledgement jobs.

#223 retains the coupled VersionSet, VersionBuilder, Version, Recovery and
Database suites. Its final original 119 cases are not the earlier incomplete
106-case selection or a subsequent integrated selection with extra cases.
Parent encoding/file lifetimes, precise error-copy sites and allocation throws
remain inspection-based where no direct observer exists.

#225 adds two snapshot/liveness cases to the same five-suite metadata selection
used by #223, so its original/final 121 is distinct from that earlier 119.
#228 uses a different diagnostic/WAL/table/native-output selection: its
literal 735-character golden covers ordered bytes 0-255, not Unicode decoding.
Its 103 and 35 counts are not metadata or older WAL batch totals.
Allocation/copy/destruction or growth/mutation observations remain
inspection-based where the detailed designs explicitly say so.

#230 adds three mixed framed-stream cases to #228's same ten diagnostic/WAL/
table/native-output suites, giving its distinct original/final 106 cases.
The literal fixtures cover physical then logical corruption, every successful
output prefix before injected failure, header-before-read suppression and fatal
later read priority. Reporter/decoded-record ownership and allocation/physical
copy behavior remain source-inspected where no supported observer exists.
The integrated selectors are not pure WAL-suite totals. Both 38-case
intermediates are diagnostic-only subsets, not alternate 106-case baselines.

#233 adds two table-boundary cases to that same ten-suite selection, producing
its distinct original/final 108. Original-source execution corrected the
accepted design's empty-data error literal: ReadDataBlock reports
`"table data block is empty"` before EnterBlock's later unreachable-entry
fallback. No production policy was changed to satisfy the mistaken premise.
Its four-way first-key/terminal-failure matrix and referenced-block header
failure preserve exact output and error/read boundaries. Parent comparator/
table/iterator and iteration-local lifetimes remain source-inspected.

#236 adds two classification/dispatch cases to the same selection, yielding
110, not an interchangeable 108 or metadata total. Exact invalid-name and
canonical unsupported errors precede every filesystem operation; literal
nested-path outputs identify all three dumpers, and acquisition beats output
failure. The tab-containing parent is in-memory evidence, not a real native
filename-validity claim. Conversion/escape evaluation order and physical
allocation/move/elision/destruction limits remain explicit in its design.

#238 adds two cases to #225's same five-class metadata selection, yielding
123, not the earlier 121. Explicit previous-log zero resets the current sparse
counter without reducing the historical bound; decreasing log still wins over
missing counters, final overlap and an armed later read. The six-definition
ledger keeps successful counter assignments before Version allocation and
builder/comparator/pointer/counter order. Failed private replay state and
physical error-copy/allocation ordering limits remain source-inspected.

#241's thirteen-class selection contains 201 unique original/final identities,
not #208's narrower 129 or any hash/cache total. Its two public reverse cases
cover owned empty/binary observations, zero comparator calls, exact corrupted
state clearing and supported positioning recovery. An explicit found output
retains successful no-predecessor semantics without adding a new valid guard.
The 42-case block-only intermediate is a subset, not an alternate baseline;
borrowed block/comparator and parser-local lifetimes remain source-inspected.

#243's nine-class hash/Bloom/filter/cache/table selection includes the separate
cache-allocation executable and yields 96, with exactly six HashTest identities
in its intermediate. Four high-seed zero/high-byte matrices preserve 80 new
literal prefix outputs plus 39 existing literals, independently cross-checked
offline without production calls. Guarded real subspans preserve pointer,
length and storage. They do not certify dead reads, huge physical inputs,
scalar-copy/elision or a direct Hash32 allocator observer. Actual hosted
Windows Debug also matches #241's full 201 selected identities, proving that
the merge context includes its two delivered block cases; the local original/
final hash runs were on the same older-block baseline. No benchmark policy or
performance-equivalence claim is inferred from those golden values.

#245's seventeen-class selection contains 238 unique original/final identities,
not a hash/cache or metadata baseline. Six original public cases cover normalized
comparison/user-order magnitude, shortening boundaries/callback order/throws,
borrowed limits and complete literal prefix/restart blocks with Reset history.
Both 35-case intermediates are exactly the original foundation subset.
SharedPrefixLength has two real callers; InternalKey shortening still appends
the max-seek trailer only after strict eligibility and publishes only after its
full-key limit comparison. BlockBuilder retains parent key-delta lifetime and
header/payload/history/restart order. Pure scalar argument sampling and physical
copy/allocation/evaluation-count limits remain explicit in the scoped design.

The first #245 GCC 13 coverage run exposed an admission-branch gap despite
1,186 executions: the compound condition had 75% (3/4), not 100% branch coverage.
A reverse start/limit boundary row was first executed and independently reviewed
on original production, then normally cherry-picked into the published branch.
The comparator's conditional separator guarantee permits this input; no new
precondition was imposed. Corrected coverage has 1,187 hits and 100% (4/4).
No production algorithm, gate threshold, exclusion or published history rewrite
was used to obtain the successful corrected exact-head checks.

#248 adds two cases to the same five-class metadata selection used by #238,
yielding 125 rather than 123. The six-row presence/bounds/topology matrix
preserves exact first-error priority. Successful/failed recovered-MANIFEST
publication observes the original snapshot, duplicate staged pointers and
captured edit counters, while live next/sequence advance under the mutex.
The raw pin is acquired during unlocked CURRENT temporary sync, retained only
after successful publication and destroyed under the same synchronization.
Failure keeps original topology and a sticky exact error without later I/O.
Weak probes and owned summaries do not accidentally retain old versions.
Both 41-case intermediates match the original VersionSet subset exactly;
allocator-failure, physical-copy and general concurrent-writer claims are not
inferred. WritePrepared's visible relock/exception policy and ReleaseRead remain
unchanged, and real replay/staging callers share compact-pointer assignment.

#250's nine-class lookup/internal-key/MemTable/table selection contains
151 corrected original/final identities. Its first published head exposed a
portable GCC/Release warning in a guarded test vector, not a production defect.
The literal leading/trailing guard construction was corrected and reviewed
first on unchanged production, then normally cherry-picked; no warning or
coverage gate was weakened. The two 36-case intermediates are exact original
foundation subsets. Lookup storage ownership, canonical moved-from state,
actual shared trusted internal-key emission and construction error priority
remain bounded scope, not whole-file certification.

#253's Result/CRC/native writer/table/database selection contains 166 unique
original/final Windows identities. The shared first-failure child borrows the
parent Status and optional slot; Error::ToString retains its parent string and
appends the optional complete message through a private child. The 26-case
intermediate is the Result/CRC subset. Existing Error factory locations,
native resource transactions and the four CRC primitives remain unchanged;
ordinary execution does not prove allocation failure or physical copy counts.

#254 is the first delivery under the owner's larger-batch policy: one accepted
local design, three original-source test commits, three production topic
commits, integrated evidence and one published implementation PR. Its 18-suite
selection contains 296 identities. Independent review found signed iterator
offsets in two new block tests; the correction was proven first on unchanged
production and then cherry-picked. Scalar, block and MemTable intermediates are
exact original subsets of 99, 197 and 131 identities. All six changed
production/test translation units compile locally with GCC Release warnings as
errors; the attempted full local GCC 16 target remains blocked by an unrelated
pre-existing compaction diagnostic, while hosted GCC 13 gates all succeeded.
This batch certifies only its eighteen selected/new definitions and bounded
codec disposition, not the complete block, MemTable, coding or production tree.

#255 applies the same larger-batch policy to the complete cache core and
unchanged typed wrapper. Its 35-case selection spans the real unit, isolated
allocation and TableCache users; hash-table and retention intermediates are
exact original subsets of 28 and 29. Design review corrected exact rehash
prepend order, replacement/growth observability, duplicate-pin timing,
same-shard reentry and destructor stale-index scope before implementation.
All 25 selected/new definitions and every unchanged core/typed runtime body
have a bounded responsibility disposition. Hosted checks and actual Windows
Debug names all succeeded. Engine TableCache loading policy and other cache
users remain separate frontiers; this does not certify whole production.

#256 closes the expanded scan's first major private-header blind spot.
Its five-suite selection contains 100 corrected original/final identities and
the node-layout/publication intermediate is the exact original 32-case subset.
The final semantic ledger contains 16 selected/new and 32 unchanged definitions;
the scanner confirms 48 definitions, zero lambdas and zero recovery nodes.
Integrated review caught a new pre-construction pointer-provenance regression:
InitializeLinks was restored to the original Arena storage pointer and the
shared LinkAddress removed before publication. The baseline post-construction
Node::LinkAt tail-allocation idiom remains an explicit portability limitation,
not a new guarantee. Hosted checks and actual Windows Debug parity succeeded;
Arena internals, MemTable's other definitions and whole production remain open.

#257's nine-suite write/WAL/recovery/database selection contains 114 corrected
original/final identities. Persistence/grouping, queue execution and buffer
generation intermediates are exact original subsets of 40, 71 and 74.
The complete bounded inventory covers 61 named definitions and the forced
fallback, wait and retained-generation cleanup lambdas. The design preserves
the original one failed-Error copy at WAL persistence and avoids adding a
second copy at the new parent boundary. Stack Writer lifetime, result allocation
before TakeOn, lock reacquisition, sequence restoration, WAL-before-MemTable
visibility and active/immutable owner order all passed independent review and
hosted gates. DatabaseEngine caller bodies and wider write/recovery files remain
separate audit frontiers.

#258 completes the trusted format bodies intentionally left open by checked
record validation and WAL I/O delivery. Its twelve-suite selection contains
144 corrected original/final identities; trusted batch and fragment-cursor
intermediates are exact original subsets of 94 and 121. The final ledger covers
59 definitions with no lambdas. Count commits only after complete record bytes;
deletion consumes no value; exact-seven-byte block tails may emit empty First;
cursor state advances before return. Checked validation, checksum/error tuples
and wal_io recovery remain unchanged boundaries. Hosted checks and actual
Windows Debug parity succeeded; other format/engine files and whole production
remain open.

#259 closes all current definitions in internal_key, Arena and MemTable while
reusing the delivered key/lookup/read-record/SkipList ledgers. Its thirteen-suite
selection contains 200 original/final identities; internal decode and Arena
intermediates are exact original subsets of 39 and 33. The final ledger covers
115 definitions. Parse remains the Error/source-location parent; failed decode
does not publish a partial borrowed view. Arena's extracted admission relies on
the proven cursor invariant and removes an unreachable redundant branch instead
of adding a test hook or coverage exclusion. Hosted checks and actual Windows
Debug parity succeeded; broader table/engine/platform files remain open.

#260 reconciles every Block/Iterator/BlockBuilder/block-format definition using
the positioning, reverse, prefix-builder and read-record deliveries. Its
fourteen-suite selection contains 208 corrected original/final identities;
footer and stored-block intermediates are exact original subsets of 121 and
133. The final ledger covers 96 definitions and removes the only padding
lambda. Review corrected ownership assertions, GCC-optimized corruption
mutation, distinct metaindex-before-index fault messages and the documented
CommitEntry publish-before-restart-validation order. Hosted gates and actual
Windows Debug parity succeeded. Table, filter, compression and engine layers
remain separate audit frontiers.

#261 closes every Bloom and FilterBlock definition. Its ten-suite selection
contains 162 original/final identities; Bloom and filter-layout intermediates
are exact original subsets of 78 and 144. The final ledger covers 34
definitions. Shared probe math preserves generated 64-bit and admitted
eight-bit domains plus the exact integer 69/100 probe policy. Layout issues keep
short/range/array/topology priority and parent-owned Error source locations
through owned and borrowed entrypoints. Hosted checks succeeded after retrying
a transient GitHub GraphQL 504; actual Windows Debug identities matched.
Table/TableBuilder/compression and broader engine/platform files remain open.

The selected N01-N09 slices are delivered, not the whole native source files.
The selected diagnostic record/table loops and file dispatch are also delivered,
not the whole dump file.
Other recursive frontiers include diagnostic
header/internal-key assemblers, remaining metadata write/release children,
cache table/list/reference operations, deeper block layout/entry
decoding, WAL fragmentation and codec helpers, and remaining engine/native
children. This is not an exhaustive
replacement for the final every-definition audit: all structured dispositions,
new helpers, private-header bodies and lambdas still participate. Benchmarks
remain outside scope.

### MANIFEST recovery

`VersionSet::Recover` composes acquisition, record replay, and finalization.
Replay composes one-record decoding, comparator validation, checked version-edit
application, pointer updates, and counter updates. Finalization composes required
field/bounds validation, checked version construction, and publication.

Use a private nested recovery helper if private member access and persistent
replay state require it. Its builder/counters belong to this recovery only;
the comparator and still-unpublished VersionSet remain borrowed. Keep VersionSet
allocation before builder construction as today. Do not expose private state or
introduce a general recovery framework.

Each record retains the exact failure priority: physical corruption, decode,
comparator mismatch, builder application, and counter monotonicity. MANIFEST
physical corruption remains fatal, unlike replayable WAL physical corruption.
Required next/log/sequence fields and largest-number bounds are checked before
building/installing the version. Do not replace the checked recovery builder
with runtime's trusted path.

### Compaction

`RunCompaction` composes per-entry processing and finalization. Per-entry
processing invokes the hook, advances grandparents even for dropped entries,
handles an output boundary, parses the key, updates history, decides retention,
appends retained entries, and advances input in exactly that order.

A small history owner may hold the existing user-key and preceding-sequence
state; its update and decision children must preserve short-circuit deeper-level
checks and update the preceding sequence even when the entry is dropped.
Grandparent advancement and per-level coverage remain monotonic operations, not
fresh searches per entry. Output opening and finishing each receive their own
recursive review. Finalization checks input failure before finishing an output,
syncs a nonempty output namespace before adding those files to the edit, and
does not add a sync for empty output.

### Table opening and lookup

`Table::Open` composes footer acquisition, validated index loading, optional
filter loading, and cache identity/owned table construction. Filter loading
composes metaindex validation/handle discovery with content reading and reader
construction. A missing filter is success, not corruption.

`Table::Get` composes index routing, optional filter rejection, pinned data-block
loading, and block lookup. A block-lookup helper returns the existing owned/copy
result, not a borrowed value that outlives its BlockReference. Preserve mapped
file backing lifetime, cache key identity, fill-cache behavior, and read order.

## Characterization and review gates

Before implementing a row, read its real callers and relevant tests, then record
concrete test cases and exact focused selectors. Existing behavior-preserving
extractions retain characterization; add missing cases first. Do not manufacture
a red test for unchanged behavior or treat last round's passing tree as proof of
a new revision.

| Area | Existing test entrypoints | Required boundary evidence |
|---|---|---|
| Dump/CLI | `tests/unit/diagnostics/dump_file_test.cc`, `dump_command_test.cc` | Exact LF text/escaping/order; continue on recoverable input corruption; output/report failure precedence; native options validated before reads. |
| Metadata | `tests/unit/metadata/version_set_test.cc`, `version_edit_test.cc`, `version_test.cc` | Malformed fields and error text/order; recovered counters; checked/trusted equivalence; linear merge; allocation before I/O; relock on status failure and throw; persist before publish. |
| Recovery/lookup | `tests/unit/engine/recovery_test.cc`, `lookup_test.cc` | Missing referenced files; ordered reservation/replay; cleanup and lock lifetime; WAL versus MANIFEST corruption policy; memory tombstones; newest-first L0; seek-charge timing and early stop. |
| Table/format | `tests/unit/table/table_test.cc`, `table_builder_test.cc`, `block_test.cc`, `compression_test.cc`; `tests/unit/format/write_batch_test.cc`, `wal_format_test.cc` | Truncated/oversized reads, filter absence, physical validation, copied/owned contents, cache pins, exact tail bytes/I/O order, sticky close priority, checked/trusted boundary, codec fallback. |
| Compaction/iterators | `tests/unit/engine/compaction_test.cc`, `compaction_picker_test.cc`, `iterators_test.cc`, `db_iterator_test.cc` | Snapshot and tombstone history; dropped-entry overlap advancement; output boundaries; equal-key direction switches; empty/error children; no duplicated/skipped records. |
| Native I/O | `tests/unit/platform/posix_file_system_test.cc`, `windows_file_system_test.cc` | Slots/handles/views released on failure/throw; copied fallback versus fatal mapping errors; close priority; failed async completion drains kernel references; platform directory-enumeration rules. |
| Cache/write/background | `tests/unit/cache/sharded_lru_cache_test.cc`, `cache_allocation_test.cc`; `tests/unit/engine/database_write_test.cc`, `database_background_test.cc` | Reentrant deleter remains outside mutex; overflow/admission and pin transfer; pending output protection; sticky error ordering; WAL close/rotation and sequence publication. |
| WAL reader | `tests/unit/wal/wal_io_test.cc`, `wal_io_native_test.cc` | Crash-truncated tails, zero markers, interruption fragment rewind, dropped-byte accounting, fragment payload lifetime, terminal errors. |

Resource transactions keep their current owners and cleanup order. Helpers
borrow when moving ownership would shorten a handle, pin, iterator, memtable,
snapshot, or mapped backing lifetime. Locks stay visible at boundaries; no
callback abstraction hides unlock/relock, publication, or failure recording.
Keep status errors distinct from exceptions and preserve first-error priority.

Design is reviewed by an independent persona before production edits.
Related responsibility-sized topics may retain the accepted design,
original-source characterization, implementation and integrated evidence as
small local commits, then publish one coherent non-draft implementation PR to
amortize CI. A separate design-only PR is optional when independent scheduling
or early owner review makes it useful, not a mandatory gate for every batch.
Implementation still receives a separate integrated review after focused
characterization passes. Review findings and publication status are recorded
honestly; local review is not GitHub approval, and a draft worklist is not
implemented work.

### Local design review

An independent design persona reproduced the syntax counts, checked the named
test paths, examined the material storage/ownership recommendations, and
confirmed that structured files are not excluded from recursive completion.
It found one unsupported prerequisite: A02 did not need C03's internal
compaction decomposition. That edge was removed. A follow-up dependency audit
also removed scheduling-only edges from the other independent extractions.
This was a bounded review of the draft, not a completed atomic-function audit,
implementation review, or GitHub approval.

A subsequent independent review checked the D01a design against the formatter,
checked batch reader, and existing characterization before any production
implementation. It found no actionable design findings. Its conclusion is
limited to this first slice; implementation still requires its own review and
exact-head CI.

## Completion criterion

The implementation goal is broader than this scan deliverable. Complete a
per-function ledger for the original definitions and lambdas and all newly
introduced helpers. Each entry is either a reviewed composition with named
children or a reviewed leaf with a responsibility-based stopping reason.
Resolve every remaining mixed-responsibility frontier, including those found
inside existing helpers and structured files. Re-scan to catch new or overlooked
definitions and platform branches.

Passing behavioral checks does not alone establish this design property;
independent code review must inspect the decomposition and leaf reasons too.
Conversely, a nice decomposition does not establish semantic parity without
characterization. Only after both are satisfied can the full production goal
be called complete. Benchmark refactoring remains outside this round.
