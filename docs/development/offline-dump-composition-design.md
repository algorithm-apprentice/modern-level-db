# Offline Dump Composition Batch

## Status and delivery boundary

Reviewed design proposal for D01b, D02, and D03 from the
[production composition plan](production-function-composition-plan.md).
No implementation of this batch is included.

The design was prepared against frozen D01a implementation head
`2b04c7b3f5983d07aa062f237a4dd51ee7970f67` while #174's CI ran. Its shared
quoted-field operation is a dependency: publish this design only after #174
merges and the design is reapplied to merged main. Implementation starts after
this design-only PR merges, not merely after local review.

The owner requests moderately larger coherent PRs to amortize CI. Deliver these
related offline-diagnostic slices in one implementation PR, using small
topic-scoped commits and focused characterization per slice. Independently
review the complete combined diff and its interactions. Small tasks do not
require one PR each; there is no arbitrary numeric PR-size target.

Do not mix engine, native I/O, benchmark, table-reader, byte-parser, compression,
filter, or byte-escape algorithm changes into this batch. The scope is private
formatting and command organization in `src/diagnostics/dump_file.cc` and
`src/diagnostics/dump_command.cc`, plus directly related tests and documentation.

## D01b: table-entry rendering

Keep acquisition, arbitrary-key traversal, checked key parsing, corruption
retention, append/error policy, iterator advancement, and final error priority
in `DumpTable`. It calls two private owned-string renderers:

- `FormatTableEntry(const InternalKeyView&, ByteView)` composes user-key,
  sequence, kind, and value fields for a valid entry.
- `FormatBadTableEntry(ByteView, ByteView)` composes encoded-key and value
  fields for a malformed entry.

Reuse D01a's `AppendDiagnosticBytesField` for user_key, encoded, and value. Add
`AppendDiagnosticNumberField(std::string&, std::string_view, std::uint64_t)` for
one unquoted decimal field and reuse it for sequence in both
`WriteBatchEntryPrefix` and valid table-entry rendering. Add
`AppendDiagnosticKindField(std::string&, ValueKind)` for the fixed kind field,
delegating value/deletion spelling to `KindText`, not WAL's put/delete spelling.

Each field helper implements one field grammar using existing string,
conversion, or escaping primitives; entry renderers coordinate fields in the
original order. Existing escaper internals remain a later recursive-review
frontier. No new public API, output abstraction, or generic formatting framework
is needed.

Valid text remains
`entry user_key='...' sequence=N kind=value|deletion value='...'`.
Malformed text remains `bad_key encoded='...' value='...'`.
Deletion entries still print stored values: this is physical-history
diagnostics, not latest-value lookup.

Parse before rendering. Fully build malformed text before remembering the
existing generic corruption, then append. An append failure or later
non-corruption I/O failure overrides remembered corruption. Continue after bad
keys as before. Owned strings are complete before append; borrowed bytes stay
within iterator/table lifetime. Keep one LF-terminated append per entry and
uncaught formatting exceptions. The numeric helper must preserve D01a's
already-characterized WAL text.

## D02: MANIFEST payload composition

Keep checked `VersionEdit::Decode`, malformed-record reporting/first-error
retention, and the record-offset header in `FormatVersionEdit`. Delegate
successful payload rendering to a named field-group composition.

Name comparator rendering, scalar-field appending, compact-pointer appending,
deleted-file appending, and added-file appending as distinct children. Private
compound-payload formatters render comparator, compact pointer, deleted file,
and new file records; group functions coordinate their iteration and appends.

Reuse quoted-byte and numeric field helpers where grammars match. Scalar
fields continue using `AppendOptionalNumber` with its distinct
`"  NAME value=N"` grammar and log/previous/next/sequence order. Structured
internal keys keep `AppendInternalKey`; `{user_key=...}` is not a quoted byte
field. Preserve optional omission, container order, one LF append per field,
and immediate return before later groups when output fails.

Do not copy containers, add callback frameworks, expose new headers, or move
error retention into pure renderers.

## D03: command composition

Use a private Help/Invalid/Dump argument classification for the existing
argument template. Preserve short-circuit order: sole --help, minimum count,
command spelling, then rejection of all option-like file arguments. Reuse
existing string-view/native-path `Matches` and `IsOption` overloads. An empty
native filename remains a dump input error, not an option.

Classification performs no file reads or output writes. `RunCommand` composes
classification, help/usage dispatch, and a named dump-files operation. That
operation retains `TrackingOutput` ownership, native path conversion, input
error continuation, error reporting before output-failure checks, and immediate
stop when either output stream fails. Preserve exit codes 0/1/2, wide paths,
help's independence from the error sink, and borrowed output lifetime.

## Characterization before extraction

Add or strengthen these public-operation cases on the unchanged implementation:

1. Use existing `BadKeyThenValidTable` to assert generic typed corruption and
   exact header, malformed-entry, and subsequent valid-entry lines as separate
   appends. Fail append indices 1 and 2 and require injected I/O to override
   remembered corruption, stopping at the failing append boundary.
2. Dump an empty `VersionEdit`: successful result and exactly the MANIFEST
   header and record-offset line, without invented defaults or file fields.
3. Extend `StopsWhenTheErrorStreamFails` with a later valid log. Inspect the
   MemoryFileSystem operation suffix as in the existing stdout-failure case;
   assert the later file is never opened after the first error report fails.

Retain existing binary escaping, empty WAL fields/final sequences, table
compression/deletion values, malformed records, canonical MANIFEST ordering,
corruption/late-I/O priority, every-section output failures, and native command
behavior. Relevant suites are `DumpFileTest`, `DumpCommandTest`,
`NativeCommandTest`, and `NativeDumpCommandTest`.

Existing valid-table output-failure tests do not by themselves establish
malformed-table corruption/output precedence. The original error-sink-failure
case uses one file and does not prove later-file suppression. Do not manufacture
red tests for these unchanged behaviors.

## Review and acceptance

Independent design review found those two characterization gaps; both are now
explicit requirements above. It otherwise found the grammars, ownership,
ordering, helper sharing, and decomposition consistent with inspected source.
This was static design review, not implementation or execution evidence.

Run the same focused selection before/after the complete refactor and verify
JUnit case sets with no failures or skips. Check formatting, the existing
warnings-as-errors build, and documentation. Obtain separate independent
implementation review of the whole combined diff, resolve findings, and require
green applicable CI on the exact published head before merge.

Use small internal commits for shared fields/table rendering, MANIFEST
composition, and command organization, but one combined implementation PR.
Neither this batch nor D01 completes the production-wide composition goal.
