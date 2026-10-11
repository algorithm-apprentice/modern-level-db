# WAL physical recovery and logical transitions

## Scope

Design accepted in #202 for W01/W02 in the
[production composition plan](production-function-composition-plan.md).
Production changes are limited to WalReader in wal_io.cc/h and directly related
characterization tests. WalWriter, fragment encoding, DecodeWalFragment,
FillBlock and public record/corruption/EOF/terminal-error categories remain
unchanged. No benchmarks, format changes or coverage exclusions.
The document/source/tests are independent of pending cache/MANIFEST work;
do not edit the shared plan concurrently.

## Physical decoding

ReadPhysical retains the block filling loop, short trailer skipping, terminal
FillBlock errors and EOF. `DecodePhysical(position, remaining)` names the
one-fragment decode/state advance using the existing DecodeWalFragment.
It computes the same physical offset before decoding.

On successful decode, advance to block end for a zero marker or to
position + encoded_size otherwise, and return the same borrowed PhysicalFragment
with original position for logical interruption rewind.
On decode failure, `RecoverPhysicalFailure(failure, offset, remaining)` advances
by encoded_size for SkipPhysicalRecord, or remaining for DropBlock, before
testing crash-tail policy. Suppress only TruncatedPayload/PayloadTooLarge when
eof_seen_; otherwise return corruption with the moved original Error and exact
dropped size/offset.

Both children return optional<PhysicalRead>, where empty means suppressed
crash-tail failure and the caller continues its physical loop. Only the parent
ReadPhysical's result exposes EOF. No second FillBlock, read, or checksum
calculation is added. Borrow failure metadata within its owning decoded result
and move Error only when emitting corruption.

## Logical transitions

ReadNext keeps terminal_error handling, physical error latching, physical EOF
clearing and physical corruption composition.
`ConsumeCorruption(WalCorruption)` adjusts dropped bytes/offset if any fragmented
record is active (including an empty First), clears partial state and returns
the corruption event. Move the event; no Error copies are added.

`ConsumeFragment(fragment)` dispatches the exhaustive trusted type to named
Zero, Full, First and Continuation transitions. Children return
optional<WalReadEvent>: empty means consume another physical fragment, not EOF.
This is a private transition result, not a replacement public WAL category.
The parent wraps an emitted event with the existing Event function.

`ConsumeZero` abandons nonempty partial payload with the original zero-marker
message, otherwise clears state and continues.
`ConsumeFull` rewinds the physical position and abandons nonempty partial
payload with the original Full interruption message; otherwise clears and
returns the borrowed block-backed logical record.
`ConsumeFirst` similarly rewinds/abandons on nonempty interruption, or assigns
scratch payload, starting offset and fragmented flag in original order.
Empty First compatibility depends on HasPartialPayload, not just active flag.
`ConsumeContinuation` emits orphan corruption if no First is active; otherwise
appends payload, continues for Middle, or clears only fragmented flag and emits
the scratch-backed record with the first offset for Last. Do not call
ClearPartial on Last: the returned span borrows its scratch payload.

AbandonPartial can return the private WalReadEvent directly instead of a public
WalReadResult; it still constructs exact error/dropped bytes/offset before
ClearPartial. This removes unnecessary Result envelopes in transitions without
adding allocation, Error copies or a generic state-machine framework.
Do not add a default branch, invalid-enum fallback or another runtime guard:
DecodeWalFragment proves the type domain.
Terminate the non-void exhaustive switch with C++23 std::unreachable after
its five named cases. This is a compiler-visible trusted-domain terminator,
not a new fallback or assertion policy. Reuse the original exhaustive-switch
coverage annotation at the moved switch; add no exclusions.

Returned payload still borrows block/scratch until next ReadNext/destruction.
Do not prefetch another fragment after producing an event or an interruption;
rewound Full/First must be returned/consumed on the next public call.
Preserve terminal error latching/copy sites and repeated EOF behavior.

## Definition ledger

| Definition | Domain responsibility |
|---|---|
| `ReadPhysical` | Compose block availability, trailer skipping and one decode until physical output/EOF/error. |
| `DecodePhysical` | Decode/advance one physical fragment through the existing format primitive. |
| `RecoverPhysicalFailure` | One safe recovery advance and crash-tail-vs-corruption decision. |
| `ReadNext` | Compose terminal state, physical reads and emitted logical transitions. |
| `ConsumeCorruption` | Interrupt active assembly and preserve combined dropped-byte provenance. |
| `ConsumeFragment` | Trusted fragment-type dispatch to named state transitions. |
| `ConsumeZero` | Zero marker interruption/reset policy. |
| `ConsumeFull` | Full-record interruption or standalone borrowed delivery. |
| `ConsumeFirst` | Fragmented-record interruption or first-payload initialization. |
| `ConsumeContinuation` | Orphan decision or continuation append/completion. |
| `AbandonPartial` | Materialize partial provenance before clearing state. |

Scratch append/assign are existing representation primitives. Rewind plus
abandon are the coupled interruption transition, not unrelated statement
wrappers. FillBlock, writer append stages and fragmenter cursor are later
recursive frontiers, not certified by this reader-only batch.

## Characterization and delivery

Existing WalReaderTest covers short reads, empty/large records, unknown-type
skip, checksum block drop, fragmented physical corruption, crash tails, orphan
and Full/First interruption, zero reset, empty First compatibility, physical
offsets, terminal I/O and invalid source/counts.
Strengthen exact corruption code/message/dropped-byte/offset assertions.
Add active-empty-First physical corruption provenance, and a
First/Middle/Full interruption chain proving combined bytes/first offset,
rewound Full delivery and no read-ahead past the current block.
Add complete-block oversized payload corruption vs final partial-block benign
tail, then recovered next-block output; use existing physical fixture builders.
Check returned scratch data before the next read instead of retaining a span
past its documented lifetime.

Run characterization on original source before production edits. Rebuild the
actual unit target and select WalReaderTest, WalWriterTest, WalIoNativeTest plus
directly coupled VersionSetTest/RecoveryTest as available by actual suite names.
Inspect exact unique before/after JUnit sets and required added cases with no
failures/errors/skips. Independent design and combined implementation reviews,
format/docs/whitespace validation, and exact-head successful remote CI precede
normal merge. Do not change thresholds or exclusions.

Independent design review found the need for an explicit compiler-valid
non-void dispatch terminator; the std::unreachable rule above resolves it
without inventing invalid-enum behavior. It found no other significant issue.
The reviewer inspected source/format/tests without editing or executing tests.

## Implementation evidence

Three new reader cases and strengthened exact physical corruption assertions
passed on original production at
`4c2cdd54f0c5da8211407a3e87115efee1a0893a` before extraction. The cases cover
active empty-First provenance/reset, combined First/Middle interruption with
rewound Full and no next-block read-ahead, and oversized complete-block
corruption versus benign final partial tail. Exact JUnit sets verify the same
67 WalReaderTest/WalWriterTest/WalIoNativeTest/VersionSetTest/RecoveryTest cases
before/after with no failures/errors/skips; the physical topic passes 25 cases.

The unpublished branch was rebased over unrelated delivered cache #200.
The four WAL design/source/header/test paths were byte-equivalent after rebase,
and the integrated unit target was rebuilt before rerunning all 67 cases.
No public category, writer behavior, Error copy, or backing owner was added.
The eleven-definition ledger above matches the implemented boundaries.
The original exhaustive-switch coverage annotation moved with dispatch;
std::unreachable is the compiler-visible terminator required by design review.
Remote exact-head coverage remains authoritative.

Independent combined review found no significant issue in the four changed
paths. It directly verified the same 67 unique cases and zero failures/errors/
skips in both JUnit artifacts without executing builds/tests or independently
establishing build/rebase provenance. Cache/MANIFEST changes were excluded.
