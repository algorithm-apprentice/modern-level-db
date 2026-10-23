# ADR-0088: Read-Efficient CLOCK Block Cache

- Status: Accepted
- Date: 2026-10-22

## Context and boundary

Actual local RocksDB cache implementations improved concurrent lookup/release
cost in the retained experiments. That result is not evidence that copying
the generic cache API, assuming replacement semantics, or using a different
implementation will have the same speed.

The shared generic LRU core also caches tables using eight-byte keys.
Do not modify that core or adopt the SIEVE experiment as the default.
Introduce a block-cache-only, self-contained backend inspired by stable-slot
CLOCK/reference metadata. Use no RocksDB production dependency.

This topic follows [ADR-0085](0085-reviewed-read-optimization-adoption.md).
Its concurrency protocol requires independent design review before code.

## Public and facade contracts

Keep the existing opaque BlockCache and capacity factory source-compatible.
Add explicit policy selection (`Automatic`, `Lru`, `Clock`) through a checked
options factory; reject an invalid policy rather than silently choosing one.
The original capacity-only factory selects Automatic.

Automatic initially remains LRU. Default selection can change only after the
production backend and facade pass review and resident/pressure/scan/shift,
one/four-reader measurements. Small/unsupported configurations may retain
LRU through a documented automatic selection rule, not an allocation-error
fallback. Explicit allocation failures propagate under the existing model.

The facade owns one backend. Its move-only lease retains either a generic
LRU handle or a CLOCK lease; it never allocates on lookup. Use proper typed
alternatives rather than pointer tagging or layout reinterpretation.
Preserve the Table-facing value/arrow/dereference and retention contract.
Measure facade overhead with explicit LRU as well as Automatic/CLOCK.

The checked public entry point is
`CreateBlockCacheWithOptions(const BlockCacheOptions&)`, returning
`Result<shared_ptr<BlockCache>>`. The options contain capacity (default zero)
and policy (default Automatic). Keep the legacy `CreateBlockCache(size_t)`
name/signature separate: even `CreateBlockCache({})` must remain an unambiguous
zero-capacity call, not become ambiguous with a new aggregate overload.

Use an in-place typed backend variant, with construction-only monostate for
the selected nonmovable backend. Its lease is a typed LRU-pin/CLOCK-handle
variant; no separate per-hit allocation is introduced. Internal CLOCK
diagnostics remain available for admission measurements, without adding a
general public metadata/RSS observation promise.

CLOCK accepts the exact 16-byte cache identity constructed by the table
layer: cache instance/file identity plus block offset. Hashing chooses shards
and buckets; comparison still uses every key byte. Generic LRU remains
available for its established full-key semantics.

Equal block-cache keys identify the same semantically immutable block, as
established by table cache IDs and immutable file offsets. A successful
lookup returns the Live generation that it actually pinned; it does not
promise the newest overlapping replacement. Sequential retained replacement
makes the new mapping visible. If replacement must be uncached, remove the
old mapping before returning the new uncached lease; later lookup may miss,
but cannot keep returning the invalidated old mapping. Existing old leases
remain valid.

Charge counts indexed block data, not erased/uncached values or allocator
metadata, matching the current public observation. Preserve unique value
ownership, move-only pins, live erased/replaced handles and out-of-lock
reclamation. Do not claim that opaque cache lookup pins provide DB snapshots.
All facade leases and lookups finish before cache destruction, uniformly for
both backends. Standalone cleanup does not relax that caller obligation.

## Representation

Use sixteen shards and the established output-stable key hash/shard rule.
Each shard has a writer mutex for admission, erase, index changes and
reclamation bookkeeping. Ordinary successful lookup/release does not take
this mutex. A speculative collision pin can become the last Erased release
and use the rare cleanup path; do not claim an unconditionally mutex-free
operation.

Canonical slots have stable addresses until cache destruction. Grow storage
in separately owned fixed-size segments; growing the segment-owner vector
never moves a slot. A slot contains:

- A uint64 atomic word: two state bits, one visited bit, 61 external-pin bits.
- The complete immutable key, cached hash, charge and typed Block pointer.
- An owning value-holder pointer and stable CLOCK position.
- Pool/standalone identity that does not change during reuse.

The owning value holder is allocated before publication and can form an
intrusive retired list. This permits retiring several victims without a
fallible allocation after invalidation or running a Block destructor while
holding the shard mutex.

States are Empty, Writing, Live and Erased. Live implies cache ownership;
pin bits count external leases only. Key/value fields are read only after
acquiring a Live pin and cannot be overwritten until pin count is zero.
The 61-bit count exceeds the number of addressable move-only handle objects;
do not sacrifice key/hash bits for visit/ref metadata.

An uncached admission uses a separately owned standalone slot with one lease,
not an indexed pool slot. Last release destroys it without accessing dead
cache state. Capacity zero, oversize/pinned pressure and metadata-budget
exhaustion can return such a live uncached lease.

## Safe lookup and pin acquisition

The current index is an atomically published pointer to an open-addressed
array of atomic canonical-slot pointers. Null means a virgin empty bucket;
a real typed sentinel represents a tombstone. No integer-as-pointer sentinel.
Hash and mask choose the first bucket; complete key comparison chooses
identity.

The existing internal facade keeps ByteView for generic LRU callers. A selected
CLOCK backend requires the exact 16-byte table key; the table's fixed-size key
construction establishes that private precondition. Do not weaken arbitrary
full-key LRU behavior or add a repeated recoverable-error path for a shape the
table caller has already established.

The acquired root owns immutable bucket count/mask alongside its array.
Never pair an old pointer with a separately updated larger mask. Initialize
the complete root before release publication; load the root and bucket
pointers with acquire ordering. Every bucket-pointer publication uses a
release store, including a new segment's slot exposed through an existing
root.

A lookup loads one index pointer, probes a bounded number of buckets and
uses CAS to change Live metadata to the same state with one more pin and the
visited bit set. Only after successful acquisition may it read key/value.
Mismatch releases that pin and continues. A non-Live slot or bounded CAS
contention can cause an optimization miss; never read an unpinned value.

Value publication is a release transition to Live. Acquire pin acquisition
and subsequent metadata RMWs form the synchronization/release-sequence
argument for immutable key/value visibility.
Indexed admission publishes with its Insert-result reference already counted:
Live/refcount-one is owned by a nonthrowing pending lease before the index
mapping is exposed or a retired destructor can run. A reentrant destructor
can erase that mapping before Insert returns without invalidating the result.

Release decrements through a release RMW. Every zero-reference cleanup,
eviction or reuse claims ownership through an acquire (or acq_rel) operation
that observes the reference-release sequence before reading/moving/overwriting
key/value storage. Live-to-Erased transitions use a count-preserving atomic
RMW/CAS, never a snapshot store that can lose a racing pin.
After decrementing, a releaser uses only permanent slot/shard identity until
it establishes serialized cleanup ownership.

Writer reuse cannot cause an ABA use-after-free: slot addresses remain
valid, CAS acquires whichever Live generation is current, and every hit
compares the pinned generation's full key. A lease prevents reuse of its
generation until its own final release.

Lookups are not claimed wait-free. Bound bucket and CAS retry work; declining
a cache hit still performs the normal checked SST read, not a missing-key
result. There is no silent data/error fallback.

The bucket limit is that acquired root's full bucket count, so same-hash
clusters cannot place retained keys beyond the search bound. Admission and
every refresh use that same full domain and cannot silently omit a Live
mapping. A bounded pin-CAS retry can miss even during reader-only contention;
the retained-key reachability promise below applies with no competing
metadata updates.

## Index growth and refresh

Writers allocate a larger index before publishing it, then rehash canonical
Live slots and release-publish the new root. Retain earlier index arrays
until cache destruction; geometric growth bounds their aggregate size.
Readers can safely finish probing an earlier array.

Do not retain one new array for every tombstone refresh. Refresh the current
atomic pointer array in place under the writer mutex, using the canonical
Live slots as the source. Concurrent lookups can miss transiently while
the index is refreshed, but cannot return an unpinned/wrong value.

Consequently this specialized optimization cache does not promise a
linearizable always-hit map during concurrent mutation. With no competing
index or pin/visit updates it must find every retained key. Existing public Get correctness does
not depend on a cache hit; misses follow the original file/CRC/decode path.

An earlier array can point to a slot subsequently reused for another key.
Stable storage, pin acquisition and full-key comparison make that safe.
Never free slot storage or an index reachable by an active lookup.

## CLOCK replacement

Scan stable CLOCK positions rather than appending new admissions after a
FIFO hand. A reclaimed position can be reused; newly inserted entries do not
indefinitely extend the hand's distance to old cold entries.

Only a Live slot with zero external pins can be reserved for eviction.
Under the writer mutex, CAS its metadata to Writing before moving its owner
or changing key/value fields. A racing reader either acquired its pin first,
preventing eviction, or observes non-Live metadata and declines the hit.

Visited entries receive a second chance by clearing the bit through metadata
RMW. Bound one admission's scan effort to a small fixed number of complete
position passes. If pins/contention leave no eligible victim, return a live
uncached lease rather than spin indefinitely.

Pinned data and cache capacity remain different concepts. Do not force
eviction of a live lease or convert retention failure into a logical Get
absence.

## Erase, replacement and reclamation

Erase invalidates the current index mapping and marks its slot Erased while
preserving existing pins. Remove its charge immediately, as erased pins are
outside indexed-cache accounting.

If there are no pins, the writer can detach the value, make the slot
reusable and retire the owner. Otherwise the last Release takes the writer
mutex only on this rare erased-entry cleanup path. It rechecks current state,
because another writer may already have reclaimed or reused the slot.
Normal Live release is one reference-count RMW, without a writer lock or list
movement.

A last releaser must not access key/value after dropping its pin unless it
successfully owns cleanup under the mutex. If the slot has been reused, it
must not delete the replacement. Reclaiming a later Erased zero-pin generation
is permissible only after confirming its index/charge invalidation is already
complete under that same writer serialization.

Replacement preserves old leases and makes the new value available or
explicitly uncached. Complete all fallible allocation/reservation preparation
before invalidating the old mapping. Sequential lookup after a retained
replacement returns the new value; concurrent lookup can transiently miss,
but must never return another key's bytes or free a retained value.

Detach/queue owners under the mutex; destroy them after unlock. Reentrant
destructors can call cache operations without self-deadlock.

## Deterministic protocol test boundaries

Use the actual production transitions for controlled schedules, not a second
cache model or conditional profiling callbacks. Keep canonical Slot/Index
declarations and pin-word constants in one private protocol header shared by
the implementation and internal tests; do not create different inline bodies
under different translation-unit flags.

Factor root acquisition from probing the acquired root. Ordinary Lookup calls
those same stages; an internal test peer can retain the typed root observation
across growth, in-place refresh and canonical-position reuse. The observation
borrows the cache, not a payload owner, and never combines its root with a
later root's mask.

Factor a single Live-pin CAS attempt from the bounded retry loop. The loop
checks Live before each attempt, and only a successful acquire CAS permits
the complete-key comparison. Tests can pause with the actual observed word
before CAS or retain the acquired pin before erase/replacement. A successful
stale-word ABA pins the current Live generation; it must still compare that
generation's complete key before reporting a hit.

Factor the release RMW from the rare serialized completion. Capture permanent
shard identity before decrementing; return that typed identity only when an
Erased last-pin transition needs cleanup. Completion rechecks current
Erased/zero-pin eligibility under the writer mutex. Tests can pause after the
actual decrement while a writer reclaims/reuses the position, including a
replacement becoming Erased/zero-pin before the earlier completion resumes.
Never carry a detached Entry or unpinned payload pointer across that gap.

The test peer may consume a lease into such a pending cleanup, but must finish
that cleanup before cache destruction, including on assertion unwinding.
These boundaries add no production runtime hook, alternate read algorithm,
allocation, metadata field or policy option. The initial protocol-factoring
gate required no added linked Lookup/normal Release helper or allocation calls;
the final scalar refinement explicitly amends the single-call boundary below.
Private source factoring alone is not evidence of either result.
Independent architecture re-review approved these boundaries, not their
implementation, facade, default selection or performance.

## Metadata and allocation bounds

Grow slot/index capacity lazily. Track segment, free-position and current/
retained-index storage against an explicit cache-creation metadata allowance;
the initial proposal is `max(64 KiB, data_capacity / 4)`, apportioned across
shards with overflow-safe arithmetic. This tracks storage payload, not exact
allocator RSS.

Count allocated capacities, not vector sizes: pool segments, segment-owner
containers, free-position storage, index root headers, current/retained bucket
arrays, and holders/Block-object metadata for currently indexed entries.
Pool storage remains counted when a pooled slot is Erased/pinned or free.
Externally retained standalone/erased value holders, their block data, and
detached retired owners awaiting out-of-lock destruction are explicit
exclusions, like existing uncached/erased retention; report their peaks
separately rather than claiming a hard process-memory bound.

Include the complete owning facade object in that fixed account, including
inactive in-place variant storage. A CLOCK member inside a large LRU/CLOCK
variant cannot report only `sizeof(ClockBlockCache)` as its owning footprint.
Its private constructor receives the compile-time known complete owner size
before publication; standalone construction supplies its own object size.
This is internal accounting input, not a public budget knob, and adds no
per-entry field or per-lookup work. Keep the same total allowance and reject
retention when the complete fixed/storage account cannot fit.
An accessible internal footprint tag constructor delegates to that private
constructor, so `variant::emplace` does not need facade friendship to call a
private constructor. Replace the old embedded-object-size term rather than
counting both it and the complete owner. Independent architecture re-review
approved this facade/accounting concretization, not an implementation or
Automatic default change.

The allowance applies to published cache-owned storage. Preparation can
temporarily hold old and proposed allocations simultaneously; compute the
post-publication account with checked arithmetic before mutation, charge
actual resulting capacities, and record preparation peaks. If the proposed
published account exceeds the allowance, release the prepared objects and
decline cache retention. Do not hide allocation failure as budget exhaustion.

Prepare allocation and its bookkeeping before publishing a changed index or
slot set. On allocation failure, retain the previous valid mapping/state and
propagate the exception; do not mislabel allocation failure as normal
admission rejection.

When the fixed metadata allowance cannot admit another indexed position,
return an uncached owner. Do not retain unbounded tombstone snapshots or grow
metadata without regard to pins/data capacity. Report actual metadata and
admission rejection along with payload hit/miss curves.

## Required evidence and review

- Complete-key collisions, replacement, explicit erase, move-only leases,
  zero/oversize capacity and live uncached owners.
- Paused reader before/after pin CAS against eviction, replacement and slot
  reuse; live pins always retain their original bytes.
- Last erased release racing writer cleanup/reuse, including immediate
  erase of a replacement before the earlier cleanup resumes.
- Old-index lookup paused across growth/refresh and slot reuse; no UAF,
  wrong-key hit or unbounded probing.
- Allocation failures at owner, segment, index and bookkeeping preparation;
  no partially published index or ownership leaks.
- Bounded all-pinned/metadata-full behavior and exact charge removal.
- Reentrant reclamation and concurrent Insert/Lookup/Erase.
- Quiescent retained-key reachability and an independent byte-budget model.
- Actual hot/scan/cyclic-shift workloads, not merely the positive uniform
  corpus or cache-hit-only microbenchmark.
- Cache destruction only after all leases/lookups finish, with no cache
  pointer left in an escaping deleter.
- Thread sanitizer/model schedules plus ordinary coverage/compatibility
  gates and original/facade-LRU/CLOCK one/four-reader Get measurements.

Independent architecture review must resolve representation, publication,
reclamation, mutation-miss and metadata-bound questions before implementation.
An independent implementation persona then reviews the concrete atomics and
decisive tests. Keep Automatic on LRU unless measured evidence justifies a
better default without a significant unaddressed adverse path.

Independent lock-free storage-architect review identified and resolved six
protocol gaps: reader-release/writer-acquire synchronization, the pending
Insert-result pin, rare mutex use by collision-pin cleanup, probe/admission
bounds, complete metadata accounting, and uncached-replacement semantics.
Re-review approved this design. It does not approve a concrete implementation
or a new Automatic default.

## Implementation and admission boundary

The own backend, deterministic production-stage tests and checked typed
facade are implemented. Independent implementation review covered the
publication/release protocol, full-key ABA checks, old-root lookup, late
cleanup after reuse, exceptional preparation, reentrant retirement and
facade allocation/lifetime contracts. Tests use actual production stages
without conditional hot-path callbacks or a second protocol model.

Allocation testing exposed a real constructor leak when a raw Diagnostics
owner was created before a later throwing Shard member. Allocate it only
after those members finish construction. MSVC debug bookkeeping can allocate
even for an empty vector: explicitly throwing construction preserves the
documented bad-allocation model instead of terminating inside a noexcept
default constructor. Cache-wide outstanding retired accounting also includes
overlapping out-of-lock destruction, not just one writer's local list.

The facade includes public-only source-compatibility/invalid-policy tests,
both policies' allocation-free lookup/release checks, exhaustive constructor
allocation failures and two real databases sharing/reopening each policy.
These establish contracts, not a performance result.

Historical v1 optimized protocol inspection found no surviving stage-helper calls in the
actual linked Lookup and handle destructor. Their observed bodies were
323 and 73 bytes, versus 275 and 24 before factoring; unused Release/Reset
functions were removed by the linker. Do not claim byte identity, zero
instruction-cache effect, or a facade speedup from this observation.

### First own production campaign

The independently reviewed `clock-production` fixture sealed source
`a61607eba77e817d15ea0e695afa4237c8fcaf7b`, with exact original reader-v4
headers/library and separate candidate headers/library. All 180 fresh timed
processes passed independent verification: 72 maintenance and 108 whole-Get
processes, six pair-balanced rounds, original/facade-LRU/CLOCK roles.
Each whole Get uses a separately verified, closed/drained clone before a
fresh point-warmed measurement clone, the same closed 47-table/million-record
image, filter-deducted logical cache budget and zero timed table opens.

Verification binds the exact approved matrix/order, complete input/plan seal,
preflight, binary, arguments, environment, affinity and fixed work. Every arm
has a canonical nonaliased physical evidence directory and a child-reported
sealed run identity matching the actual launched PID; OS PID reuse alone is
not rejected. Independent review caught missing arm binding and duplicate
process-evidence admission before sealing. Twenty-one regression cases include
actual final-verifier rejection/publication tests; no accepted timing used the
earlier draft.

The following are paired median wall-time improvements, positive faster,
with all six rounds retained. Faster-pair counts are observations, not a
preset acceptance threshold.

| Logical cache | Readers | Reader reuse | Facade LRU vs original | Faster pairs | CLOCK vs original | Faster pairs | CLOCK vs facade LRU | Faster pairs |
|---:|---:|---|---:|---:|---:|---:|---:|---:|
| 8 MiB | 1 | off | -1.14% | 3/6 | -6.06% | 0/6 | -4.23% | 1/6 |
| 8 MiB | 4 | off | +0.46% | 3/6 | -3.66% | 0/6 | -3.49% | 0/6 |
| 512 MiB | 1 | off | +1.80% | 5/6 | +6.39% | 6/6 | +4.82% | 6/6 |
| 512 MiB | 4 | off | -0.57% | 1/6 | +2.32% | 5/6 | +5.83% | 5/6 |
| 8 MiB | 4 | on | -0.86% | 2/6 | -5.65% | 0/6 | -4.90% | 0/6 |
| 512 MiB | 4 | on | -0.98% | 2/6 | +6.92% | 4/6 | +7.08% | 6/6 |

The facade is not free. Its whole-Get control is mixed, and its maintenance
control is slower than the original by 15.86%/13.86% for one/four-reader hot1
and 11.34%/2.18% for uniform16,384. Do not claim universal default-path
preservation merely because Automatic still selects LRU.

CLOCK maintenance beats the original by 56.50%/74.21% for hot1 and
33.33%/72.03% for uniform16,384, with six faster pairs in every cell. The
four-reader hot1 runs also report 873, 477, 717, 874, 449 and 2,140
optimization misses per four million attempts; other maintenance cells report
zero. These are attempted query checksums plus successful owner checks,
not successful data/fallback work on misses. They exclude real SST fallback,
CRC and decode, and cannot substitute for the adverse whole-Get results.

### Own policy curves and default boundary

Actual deterministic cache replays use recorded logical block charges with
small identity Blocks. Every hit checks the complete identity. Original and
facade-LRU counts match in every phase; each policy makes its own admission
decision from the trace's retainable/fill settings, not its recorded LRU hit.
The following miss counts are policy observations, not CPU timings, physical
buffer memory, device I/O counts or RSS.

| Trace phase | Logical cache | LRU misses | CLOCK misses |
|---|---:|---:|---:|
| Recorded timed demand | 8 MiB | 1,578,400 | 1,632,673 |
| Recorded timed demand | 512 MiB | 0 | 0 |
| Hot return after scan | 8 MiB | 1,024 | 10,820 |
| Shifted hot set | 8 MiB | 1,024 | 39,464 |
| Hot return after scan | 512 MiB | 0 | 0 |
| Shifted hot set | 512 MiB | 1,024 | 1,024 |
| One-shard cyclic demand | 8 MiB | 7,808 | 719 |
| One-shard cyclic demand | 512 MiB | 0 | 0 |

Keep Automatic on LRU. Resident maintenance/Get benefits do not override the
repeatable pressured Get losses or hot/scan/shift miss curves. Explicit CLOCK
can target a suitable resident workload, but is not universally superior.
Do not ascribe the complete miss-curve difference to one unmeasured mechanism.

### Remaining admission work

The integrated source passes 1,097 current unit-label cases, 19 extended cases
and 13 CMake package/consumer checks. Rebuilding only the main/public test
targets initially left the separate allocation executable stale; comparing
JUnit identities caught seven omitted new cases before any accepted timing.
The complete build and rerun include them.

The first ordinary CI head passed 19 jobs, including ASan and TSan; the
changed-branch coverage gate alone failed. Retain that failure and the complete
first campaign. It requires meaningful deterministic control-path evidence,
removal of proven redundant private checks where appropriate, and ordinary
exact-head CI before admission. Any production revision needs its own fresh
matched campaign; do not pool revisions or silently reuse the first result
as evidence for changed code. The RocksDB prototype remains historical context,
not a substitute for either own campaign.

### Reviewed deterministic control-path follow-up

Independent architecture re-review approves the following bounded refinement
before implementation. Keep v1 intact and Automatic on LRU. Introduce no
runtime callback, profiling flag, alternate algorithm, public setting or
per-entry field.

Continue the actual peak-update CAS loop from a supplied observed value.
Production loads the same atomic before continuing; tests place an intervening
publisher between observation and continuation. Preserve relaxed ordering and
monotonic publication rather than relying on an OS schedule to lose a CAS.

Continue actual pin acquisition from its observed word and remaining attempt
budget. Production starts with eight attempts; tests can resume with zero or
one remaining. Check Live before each strong CAS, consume rather than reset
the budget, and never access a payload without successful acquisition.

Factor a clock position's metadata transition from serialized retirement.
The stage returns Skipped, LiveClaim or ErasedClaim. The writer remains
exclusive through that stage and retirement: LiveClaim still needs index/
charge removal; ErasedClaim was already invalidated. A stale unvisited
observation followed by an ordinary reader pin must decline the claim while
that reader is still retained. Do not turn this into an unpinned payload
observation.

Continue the actual probe loop with the same acquired root/key, next bucket
and remaining probes. Pass only the remaining count and derive the bucket
as `(hash + bucket_count - remaining) & mask`, coupling progress by construction.
Production starts at `hash & mask` with `bucket_count` remaining. Never reload a different root
or mask. Tests use a real acquired root and production admission/erase/refresh
to establish successive nonempty observations, ending at a real tombstone
with one probe left. Zero remaining performs no bucket load or pin. A
fabricated full index must not masquerade as the writer's current root:
every stable writer state retains virgin buckets, although refresh/churn can
make every successive observation of a concurrent probe nonempty.

Two additional complete-cache cases exercise real resource boundaries:
retain erased handles until their stable pool storage alone exhausts the
metadata allowance, and pin all eight canonical positions of a full shard
so Reserve itself must decline. Keep the existing valid uncached leases and
unchanged old values in both cases.

Remove only proven redundant private guards. For current bucket count B,
pointer-bucket bytes P, root-header bytes H and resident-owner bytes E,
growth requires B/2 existing residents. With `E >= 2P`, committed accounting
already gives `H + 2BP <= H + BP + (B/2)E <= allowance`; the old single-array
budget rejection cannot fire. An actually allocated pointer array also
establishes representable doubling. Assert these layout assumptions and the
initial eight-bucket/segment minimum fit at compile time. The complete
post-preparation Fits check remains authoritative.

Writer Find holds the shard mutex and uses the current root. Completed
insertion leaves `live + tombstones <= 3B/4`; erasure preserves that sum.
Only Live mappings remain indexed. Its duplicate state test and full-domain
failure exit can therefore be removed, with the virgin-bucket invariant
documented. Concurrent Lookup keeps its bounded continuation.

The two GCC13 implicit null-deletion exits can receive exact `1/2`
branch-only markers under ADR-0019: valid member invocation makes the
Diagnostics receiver nonnull, and DeleteEntry already dereferences its
owned argument. Record these ownership reasons on the same lines. Exclude
no real contention, allocation, budget or probe path.

Require test-first implementation, independent implementation review,
actual linked hot-path inspection, ordinary coverage/sanitizer gates and a
fresh complete 180-process campaign for the revised production code.
Independent architecture re-review approved the scalar remaining-count
refinement after the aggregate progress interface left a real optimized
wrapper call. Approval is not an inlining guarantee; preserve original
Lookup-to-LookupInRoot routing and inspect the actual revised linked image.

The scalar refinement still leaves one CLOCK-only Lookup-to-LookupInRoot call
in optimized MSVC output. Independent architecture review explicitly amends the
initial no-added-call gate for this one shared, auditable loop boundary.
Inspect both caller and callee in the final Get/maintenance PE images; this
does not permit new allocation, Pin/Drop-stage calls, duplicated algorithms,
force-inline flags or a zero-overhead claim. The 323-byte no-wrapper-call
inspection belongs to v1, not the final candidate. Fresh measurements include
the actual extra call and any instruction-cache effect. Implementation review
approves the scalar continuation's correctness, not its performance.

The revised source's second coverage report leaves only two resource branches:
the storage-only side of Fits, and unpinned replacement when the canonical
free list is empty. Complete these with tests, not another production change.
The erased-pin fixture owns a real 4 KiB inactive member and passes the
complete containing object's size, as the facade does. The replacement case
fills eight positions without pins, replaces an existing key in place, and
checks pool size, all other values and charge. Independent implementation
review approves these valid owner/pool schedules; authoritative coverage is
still required.

The current tests pass 1,104 unit-label cases; the revised production source
also passed 19 extended cases and strict GCC checks. Actual final Get and
maintenance PE images match the inspected COFF caller (130 bytes), outlined
LookupInRoot (317 bytes) and real handle destructor (73 bytes). Only the
reviewed wrapper boundary remains; pin/drop stage and allocation calls do not.
The distinct v2 preflight passes all 50 cases and reproduces every phase
vector of all 18 v1 policy replays. Final v2 timing and exact-head CI still
determine admission.
