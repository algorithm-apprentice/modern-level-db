# WAL append and block-fill progress composition

## Scope

Continue the recursive WAL audit after reader transition delivery in #204.
Change only wal_io.cc/h, direct WAL characterizations and this document.
No fragment codec/cursor redesign, caller sticky-error policy, terminal-error
policy, public API, owned/borrowed record lifetime, buffering or sync changes.
This source scope is independent of #207/#208 and native opening design #209.
Avoid the shared ledger while another implementation owns it.

## Serialized record writing

AddRecord keeps cursor creation before reopened-block padding. Cursor::Next
still advances fragmenter state before each fragment's I/O; do not precollect
fragments, move padding before cursor creation, or reset cursor state on failure.
The writer itself does not latch errors: callers must stop after I/O failure.

`AppendPadding(size)` owns the static zero block and appends its bounded prefix.
This is the shared zero-padding representation primitive, not a generic fill
API. Both inputs are proven by reopened-block arithmetic or the fragment cursor;
do not add another runtime bound check or assertion.

`PadReopenedBlock()` preserves no-op when padding is zero, append otherwise,
and zeroes reopen_padding_ only after successful append. Failed or thrown append
leaves it unchanged, without promising general AddRecord retry safety.

`WriteFragment(fragment)` composes optional physical padding, header append and
nonempty payload append in that order. File Append is already a domain primitive;
do not wrap header/payload appends in meaningless single-call helpers.
Remove the existing private Append forwarding wrapper and invoke file_->Append
at the representation leaves instead.

AddRecord composes reopened padding, cursor-next/write loop and exactly one
file Flush after every fragment succeeds. No Flush on append failure, no extra
empty-payload append and no implicit Sync/Close. Preserve mutable Status implicit
moves at original failure sites; new parent envelopes also move.
Sync/Close forwarding is the existing public operation, not a new helper.

## Physical block filling

FillBlock still returns false immediately if EOF was already seen, without
changing offsets or reading. Its successful outcome is block_size_ != 0,
including a nonempty final partial block.

`BeginBlock()` advances block_start_offset_ by the old block_size_, then resets
block_size_ and block_position_ in original order. These coupled coordinates
define one transition to the next block, not independent generic setters.

`ReadBlockChunk()` constructs the unused block span, invokes one SequentialFile
Read and validates that its returned count does not exceed that span.
Return Result<size_t>, preserving the original read.error() copy and exact
oversized-read Error::Io text. It neither publishes the count nor changes EOF.
New parent error transfer moves the Result error rather than copying it again.

`AcceptBlockChunk(count)` publishes one checked read result: zero sets eof_seen_,
positive increments block_size_. Do not count a failed or oversized read as
progress, erase a partially filled block on error, or set EOF on a short read.
Its input is already checked, not another public boundary.

FillBlock composes BeginBlock and bounded read/accept iteration, breaking only
on zero count and returning whether any bytes were filled. Requests shrink with
accepted progress; a complete block causes no speculative EOF read.
ReadPhysical and ReadNext retain their existing error-copy/latch and partial
record policies. New children do not prefetch later blocks or invalidate emitted
record bytes sooner. An already terminal ReadNext never invokes FillBlock again.

## Definition responsibility ledger

| Definition | Responsibility or composition boundary |
|---|---|
| AppendPadding | One static-zero prefix append representation. |
| PadReopenedBlock | One pending reopened-block padding transaction. |
| WriteFragment | Ordered physical padding/header/nonempty payload composition. |
| AddRecord | Reopened boundary, incremental fragment writing and final flush. |
| BeginBlock | Coupled next-block coordinate transition. |
| ReadBlockChunk | One bounded native read/count-validation transaction. |
| AcceptBlockChunk | Checked count-to-progress/EOF state transition. |
| FillBlock | EOF gate, next-block setup and bounded fill composition. |

The removed forwarding Append is not replaced by wrappers of file operations.
Fragment Cursor::Next, physical decoder and smaller logical transition helpers
remain separate recursive frontiers; this batch does not certify every WAL
function merely because parent loops become compositions.

## Characterization, review and delivery

Existing cases cover every padding/header/payload failure, reopened/empty records,
streaming fragments with one flush, short reads, zero/oversized counts, EOF,
terminal failures and corruption/tail boundaries.

Before extraction add a multi-block append-failure matrix checking exact attempted
append sizes, persisted physical prefix bytes, original error code/text, no later
append and no Flush. Derive expected physical records using the test encoder
rather than the production fragment cursor. Include success and empty payload
controls; do not claim retry behavior after a cursor has advanced.

Record requested read capacities in the existing test-local sequential state.
Add full-block then partial-block/empty-record characterizations with deterministic
short reads: exact request shrink sequence, offsets, complete-block no EOF
prefetch, final-partial read to zero and repeated EOF without new reads.
Add failure after successful partial chunks: no partial logical record published,
exact terminal error and no further read after repeated ReadNext.
Oversized-count code/text and no reread remain required.

Run characterizations on original production before the separate writer and
reader progress topic commits. Rebuild the actual unit target and run WalWriterTest,
WalReaderTest, WalIoNativeTest, VersionSetTest, RecoveryTest and DatabaseTest.
Verify exact unique original/final JUnit sets and required new cases, with no
failures/errors/skips. Maintain all formatting/docs/whitespace and coverage gates;
no new exclusions or lowered thresholds.

Independent design review precedes accepted design merge. Independent combined
review follows local parity and precedes implementation publication; reviewed
exact-head CI must succeed before normal merge and content verification.

Independent bounded design review found no significant issue in the eight
responsibilities, cursor/I/O order, borrowed ownership, block coordinates,
error-copy/latch preservation or characterization feasibility. It did not edit
files, execute builds/tests or establish implementation parity/runtime behavior.
