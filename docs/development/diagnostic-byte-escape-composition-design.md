# Diagnostic Byte-Escape Composition

## Status and scope

Proposed recursive follow-up to the diagnostic formatter slices in the
[production composition plan](production-function-composition-plan.md).
Source-read baseline: `bdbb01d`.

Scope is `EscapeDiagnosticBytes` and two file-local children in
`src/diagnostics/dump_file.cc`, characterization in its existing unit tests,
and this document. Metadata snapshot/liveness changes a different source,
test and design file, so this design is independent of its implementation.
No public/private header, dump traversal/record policy, native output,
filesystem, CLI, coverage exclusion or benchmark change.

## Preserved byte representation

The operation is byte-oriented, not Unicode decoding. Empty input produces an
empty string. Apostrophe, backslash, LF, CR and tab use their exact existing
two-character escapes. Other bytes in inclusive range 0x20-0x7e are literal.
Everything else, including DEL, NUL and each non-ASCII UTF-8 byte, uses lowercase
four-character `\xhh` spelling. There is no surrounding quote or final LF.

Keep one parent-owned result string and the original initial `reserve(size)`.
Iterate in input order without copying the input or precomputing output size.
For hexadecimal bytes preserve the original three string mutations: append
the `\x` prefix, push the high digit, then push the low digit. Do not combine
them into a new array/string append, change growth policy, add another
allocation or catch allocation failure. Existing path/error/key/value callers
continue to use the same public function and retain their own output policy.

## Three-definition recursive ledger

| Definition | Responsibility and stopping reason |
|---|---|
| `EscapeDiagnosticBytes` | Own/reserve the result and compose ordered per-byte rendering. |
| `AppendEscapedDiagnosticByte` | Convert one `std::byte` to its bounded unsigned value and select its existing representation. Special/literal string appends are existing primitives; hexadecimal rendering delegates to its child. |
| `AppendDiagnosticHexByte` | Render one proven 0-255 value as prefix plus two digits. Those stores jointly encode one fixed-width representation and are a leaf. |

Place both children beside the existing file-local HexDigits table.
They borrow the parent string, return void and create no temporary owning
buffer. The hexadecimal helper has only the bounded per-byte caller; do not
add redundant runtime range guards or a new public helper.
No wrapper merely renames `push_back`, `reserve`, `to_integer` or table indexing.

On the supported eight-bit platforms, the unsigned byte conversion yields
0-255, so shifting by four and masking with 0x0f each produce a 0-15 table
index. Helpers retain neither reference; the parent string and caller-owned
input backing stay live for the entire synchronous traversal.

Other formatter, WAL/table traversal and report/error-precedence definitions
remain separate recursive frontiers, not certified by this slice.

## Characterization before extraction

Use the existing public escape function without a new seam.
Add an exhaustive ordered 0-255 input with independently specified literal
golden output, covering all escape alternatives, inclusive ASCII bounds,
DEL and every high byte. Do not construct the oracle with the production
function or an equivalent switch/hex-digit loop.

Add empty input and mixed/repeated special, NUL and UTF-8 byte sequences.
Check exact output, no actual LF/CR/NUL and no Unicode interpretation.
Existing dump tests already observe LF framing, escaped path/error/key/value
fields and output failure precedence; retain them unchanged.
Allocation call ordering/throws remain inspection-based, not claimed by output
equality or an unsupported allocator injection.

Run the actual discovered DumpFile, DumpCommand, NativeCommand,
NativeDumpCommand, WindowsOutput, WalReader, WalWriter, WalIoNative, Table and TableNative
native Windows selections on unchanged production after adding the cases.
Record unique classname/name sets and actual counts from XML; platform-native
tests do not exist on every OS, so do not reuse a Windows count as a POSIX count.
Require the same selected identities after extraction, all additions and no
failures/errors/skips. Existing hosted native suites cover each platform.

The exact anchored Windows selector is
`^(DumpFileTest|DumpCommandTest|NativeCommandTest|NativeDumpCommandTest|WindowsOutputTest|WalReaderTest|WalWriterTest|WalIoNativeTest|TableTest|TableNativeTest)\.`
Capture the post-characterization/pre-extraction XML as the original identity
set and compare it with final XML, rather than reusing an earlier batch.

## Topics, review and delivery

Independent design review and acceptance precede production changes.
Independent characterization review precedes extraction.
First move per-byte selection into its child while retaining hexadecimal
stores there; then extract the fixed-width hexadecimal child as a separate
responsibility-sized source commit. The final ledger is the three definitions
above, not a statement-count exercise.

Check the smallest diagnostic selection between topics and the coupled
selection at the end. Independent combined review compares the accepted
baseline, exact byte/growth/mutation ordering and scope before publication.
One non-draft implementation PR may contain both source topics.
Require all exact reviewed-head hosted checks, unchanged authoritative
coverage policy and normal merge, then verify reviewed changed paths in main.
This is not completion of the diagnostic file or production-wide audit.

## Independent design review

Bounded read-only review found no significant design issue and confirmed the
three responsibilities, preserved mutation ordering, feasible characterization
and disjoint metadata paths against unchanged `bdbb01d`.
Two documentation suggestions are incorporated: explicit bounded-nibble and
borrowed-lifetime proofs, and the exact anchored selector including the two
existing TableNativeTest cases. The reviewer performed no edits/builds/tests
or allocation-failure experiments.
