# Diagnostic File Dispatch Composition

## Status and scope

Proposed recursive follow-up to the
[production composition plan](production-function-composition-plan.md).
Source-read baseline: `2704d2e`, after record delivery reconciliation #234.
Table traversal implementation #233 is pending; its changed definitions are
outside this design. Characterization waits for that delivery and starts on
unchanged dispatch production.

Scope is `DumpFile` and two file-local children in
`src/diagnostics/dump_file.cc`, existing public dump tests and this document.
No path/filename parser, table/WAL reader, public header, CLI/native output,
filter/cache, instrumentation, coverage policy or benchmark change.
Header/internal-key assemblers and all unchanged definitions remain separate
recursive frontiers.

## Ownership and evaluation boundaries

Keep the original parent order: full-path PathUtf8, filename-component PathUtf8,
canonical filename parsing, full-path escaping, then recognized-type dispatch.
Both conversion Results remain parent locals through the dispatch call.
Do not replace them with ParseNativeFileName, filesystem normalization, early
type filtering or a new resource/owner envelope. The existing temporary
path.filename() remains alive throughout its PathUtf8 call.

A checked filename child borrows the parent's UTF-8 filename, calls existing
ParseFileName once, and retains its original const optional ParsedFileName.
An absent name returns the existing InvalidArgument
`"file name is not canonical"`; a present name transfers the simple arithmetic/
enum ParsedFileName value to Result. This public struct owns no string/view.
The parent owns that mutable checked Result and moves a failed Error explicitly.
This is a new checked-result boundary, not a claim of identical physical
move/elision counts. It introduces no replacement parser, new catches, default
type or changed canonical spelling rules.

Only canonical success permits EscapeDiagnosticBytes on the full path.
Its const string stays in the parent throughout dispatch. Canonical unsupported
types still reach dispatch **after escaping**, not before. The dispatch child
borrows filesystem, original path, parsed filename, escaped path and output.
Keep the exact existing switch, labels, formatter function identities and direct
returns:
Log -> `"log"`/FormatWriteBatch; Descriptor -> `"manifest"`/FormatVersionEdit;
Table -> DumpTable. Lock, Current and Temp return the existing InvalidArgument
`"file type is not dumpable"` without opening/reading/emitting anything.
Keep the existing exhaustive default fallback and coverage annotations; neither
forge enum values nor add exclusions to cover private impossible input.

Underlying acquisition/header/output/status behavior remains in the existing
selected dumper. There is no extra owner, retry, catch, FileSize, open, close or
borrowed escaped-path lifetime change. Full-path conversion errors retain
priority over filename conversion/classification, then canonical errors before
escape/acquisition; supported-type acquisition remains before header output.
Where no portable deterministic conversion-failure observer exists, preserve
and inspect the existing branches rather than inventing a seam.

## Three-definition recursive ledger

| Definition | Responsibility and children/stopping reason |
|---|---|
| `DumpFile` | Compose existing full/component conversion, checked canonical classification, byte escaping and borrowed dispatch while retaining parent string/result owners. |
| `ParseDiagnosticFileName` | Convert existing canonical ParseFileName's absence/presence into the diagnostic checked result and exact invalid-name error. |
| `DumpRecognizedFile` | Select the existing named log/MANIFEST/table dumper or exact unsupported-type error over the already validated enum. |

Primitive projections, ParseFileName, PathUtf8 and EscapeDiagnosticBytes already
have named responsibilities; do not add renaming wrappers around them.
The classification child adds the actual diagnostic checked boundary, while
dispatch is a separate stable domain choice, not hidden acquisition logic.

## Characterization before extraction

Retain all existing supported dump/header/output cases and unsupported-name
coverage. Add two public cases using existing MemoryFileSystem/StringOutput
and assembly/writer helpers, not new production seams:

1. Canonical non-dumpable names versus malformed/noncanonical names fix exact
   InvalidArgument messages, empty output and an unchanged filesystem operation
   log. Cover CURRENT, LOCK and canonical Temp separately from bad padding,
   zero numbers, unsupported suffix/spelling and empty/trailing components.
   Arm a filesystem failure to prove classification does not consume it.
2. Supported canonical log/MANIFEST/table leaf names beneath a noncanonical,
   escaped/UTF-8 parent fix exact type/path headers and minimal visited contents.
   Inject the selected dumper's initial acquisition failure together with an
   output failure and require acquisition to win, exact operation prefixes and
   no output. Keep path spelling expectations literal rather than computing the
   oracle with PathUtf8/EscapeDiagnosticPath/production formatters.

Use the established anchored coupled native Windows selector:
`^(DumpFileTest|DumpCommandTest|NativeCommandTest|NativeDumpCommandTest|WindowsOutputTest|WalReaderTest|WalWriterTest|WalIoNativeTest|TableTest|TableNativeTest)\.`
Measure actual unique classname/name identities after additions on unchanged
production; require additions and no failure/error/skip. Compare final identities
exactly. Do not reuse earlier record/table case totals as a new baseline.
Logical borrowed-owner validity and conversion/escape evaluation order are
source-inspected where output and operation counts cannot observe them.

## Review and delivery

Independent bounded design review precedes a design-only non-draft PR.
Design acceptance and table traversal delivery precede original-source
characterization; independently review the cases before any source change.
Use two small topics: checked filename classification, then borrowed dispatch.
Run the diagnostic-only subset between topics and the coupled selector finally.
One non-draft implementation PR may contain both.

Independent combined review checks the accepted-baseline diff, complete parser/
path/dumper call paths, exact failure and mutation order, owned strings, simple
parsed result and borrowed dispatch arguments, original annotations and literal
oracles. Record actual evidence and inspection limits.
Require all exact reviewed-head checks, actual hosted Windows Debug selected
test-name parity, normal merge and byte-exact reviewed changed paths in main.
Docs-only acknowledgements are not runtime evidence. This batch does not
certify the rest of the dump file or the final every-definition production audit.

## Independent design review

Bounded read-only design review found no significant issues. It made no edits,
ran no builds/tests and delegated no further work. This is a source-inspected
design conclusion, not runtime proof; pending table delivery and subsequent
characterization/implementation remain unverified.
