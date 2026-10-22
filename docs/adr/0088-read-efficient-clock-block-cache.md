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
