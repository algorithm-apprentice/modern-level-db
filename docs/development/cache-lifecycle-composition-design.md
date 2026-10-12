# Cache Index and Retention Lifecycle Composition

## Scope and delivery

Design-first follow-up to the production function composition plan.
Source baseline: `b37c3faef6313517f25565145a08373d04343c07`.
K01 cache admission is already delivered and remains unchanged.
This larger related batch covers the remaining ShardedLruCacheCore hash-table,
retention-list, pin/reference, eviction, accounting, entry-construction and
destruction composition, plus a bounded disposition of the unchanged typed
wrapper definitions.

Keep the reviewed design, original-source characterization and three production
topics as small local commits, then publish one non-draft implementation PR.
Do not create a PR or full CI run per helper. Changed production paths are
src/cache/sharded_lru_cache_core.cc and its private header. The existing typed
src/cache/sharded_lru_cache.h is inspected but unchanged. Direct public tests
remain in sharded_lru_cache_test.cc and cache_allocation_test.cc; TableCacheTest
is the coupled real user.

No public API, hashing, capacity, key storage, value type, charge policy,
allocation strategy, mutex domain, exception policy, benchmark, runtime assert,
diagnostic conditional, coverage exclusion or threshold change. Engine
TableCache loading/opening is outside scope. The cache must still outlive every
pin, including uncached pins.

## Existing ownership and synchronization

Each shard owns one hash table and two intrusive lists under its mutex.
An indexed entry has one cache reference. Each Pin adds one external reference.
Indexed entries with only the cache reference are in lru; indexed pinned entries
are in in_use; erased pinned entries are in neither. Table removal and list/
accounting/reference transitions occur while the shard mutex is held.

During ordinary operations, retired entries are unreachable from the table
before reclamation. Their user deleters always run after the shard mutex is
released, because deleters may reenter the cache. Preserve this for insertion
replacement/eviction, explicit Erase, and final Pin release. Destructor teardown
is different: it no longer uses the table, and baseline retirement overwrites
next_hash while stale bucket pointers still exist. Value deleters must not call
back into a cache whose destructor is running. Bucket growth allocation remains
before any table, list, reference or usage mutation. A failed allocation leaves
all mappings and accounting unchanged.

CreateEntry owns transferred value cleanup before raw entry allocation, installs
the entry deleter before releasing PendingValue, and then copies the complete
binary key into trailing storage. Its PendingEntry owns every later unwind.
No helper may publish a pointer before ownership is established or introduce
another value owner.

## Topic one: hash identity, insertion publication and rehashing

Add Entry::Matches(key, hash), one identity leaf: compare the full hash first,
then exact binary key bytes. HandleTable::Lookup and FindPointer share it while
retaining read-only versus pointer-to-link traversal policies. Hash mismatch
must avoid key comparison; equal hashes with unequal keys remain distinct.

CommitInsert retains the parent PreparedInsert, current length and no-mutation-
before-allocation contract. Extract PrepareGrowth(growth_length), returning no
owner for zero or allocated/zeroed buckets for the original doubled length.
Extract LinkEntry(prepared, entry), which publishes the replacement/new chain
link and increments elements only for a new key. It returns the replaced entry.
Only after both stages does the parent call RehashInto when growth exists.

RehashInto retains ownership replacement and final length publication. Extract
RehashChain(entry, buckets, length), which relocates one old collision chain
using the exact existing traversal-and-prepend algorithm. The parent walks old
buckets in ascending index order, and each visited entry is prepended to its new
bucket. Therefore an equal-hash chain `First -> Second` becomes
`Second -> First`; an append-based relocation is not equivalent. Then install
the complete array. Do not allocate per entry or run key/value deleters.

## Topic two: mapping, list and pin transitions

Add MoveToList(list, entry), the shared intrusive transition that calls the
existing Remove then Append operations. Ref uses it only when the first external
pin moves an evictable entry from lru to in_use, then increments refs. Unref
decrements first; zero retires, while the last external release of an indexed
entry moves it from in_use to the most-recent end of lru. Earlier pin releases
must not change list position.

Add LookupAndRef(shard, key, hash), requiring the parent-held shard lock. It
performs the table lookup and, only when present, applies Ref. Public Lookup
retains hash/shard selection, visibly scoped locking, optional result and Pin
construction. No pointer or Pin may escape without its reference.

Add EraseMapping(shard, key, hash, retired), also requiring the parent-held lock.
It composes HandleTable::Remove with existing FinishErase. Public Erase and
eviction share this exact mapping-removal/cache-reference policy.
Add EvictOldest(shard, retired), which selects lru.next and delegates one removal.
Evict retains the over-capacity/available-entry loop. Pinned-only overcapacity
still remains admitted because an empty lru stops eviction.

FinishErase retains its null gate, list removal, in_cache clearing, usage
subtraction and Unref. Retire remains the retired-chain link operation.
DeleteRetired remains the unlocked iterative deleter. Release retains its
visibly scoped mutex around Unref and calls DeleteRetired only afterward.

## Topic three: capacity, charge, entry and teardown composition

Add ShardCapacity(total), the LevelDB-style ceiling division leaf used by
Initialize before assigning every shard. Zero, exact multiples and remainders
retain the original result.

Add TryAddCharge(total, usage), one saturating aggregate step. total_charge keeps
its per-shard lock acquisition and early saturation return; it must not lock
later shards after saturation or count uncached/erased retained values.

Add CopyEntryKey(entry, key), one complete binary copy into trailing entry
storage. CreateEntry retains raw allocation, construction, PendingEntry
installation, transferred-value release, key copy and owning return in that
order. The helper is not noexcept because the parent still owns unwinding even
though byte copy is expected not to throw.

Add DestroyShard(shard, retired), which walks the original lru list with the
saved next pointer, clears cache ownership, releases the cache reference and
zeros indexed usage. Destroy retains the outer shard order and performs all
DeleteRetired calls after every shard's lru entries are retired. It does not
remove stale table links, because the table is never consulted again during
baseline teardown. The documented precondition that no Pins outlive the core
remains unchanged; destruction adds no locks or runtime validation, and value
deleter reentry during destruction remains outside the contract.

## Selected twenty-five-definition ledger

| Definition | Composition or stopping boundary |
| --- | --- |
| Entry::Matches | New hash-first exact binary identity leaf |
| HandleTable::Lookup | Read-only chain traversal through Entry::Matches |
| HandleTable::FindPointer | Mutable link-position traversal through Entry::Matches |
| HandleTable::PrepareGrowth | New optional precommit bucket allocation |
| HandleTable::LinkEntry | New chain publication/elements update transaction |
| HandleTable::CommitInsert | Growth preparation, link publication and optional rehash parent |
| HandleTable::RehashChain | New one-chain relocation leaf |
| HandleTable::RehashInto | Complete old-bucket traversal and new-array publication |
| MoveToList | New shared intrusive remove/append transition |
| Ref | First-pin list promotion followed by reference increment |
| Unref | Reference decrement, retirement or last-pin LRU transition |
| LookupAndRef | New locked mapping-to-reference transition |
| ShardedLruCacheCore::Lookup | Hash/shard/lock/optional Pin orchestration |
| EraseMapping | New shared table removal and cache-reference release |
| ShardedLruCacheCore::Erase | Hash/shard/locked erase then unlocked reclamation |
| EvictOldest | New oldest-LRU mapping removal |
| Evict | Capacity loop over one-entry eviction children |
| ShardCapacity | New per-shard ceiling-capacity leaf |
| Initialize | Capacity derivation then shard assignment parent |
| TryAddCharge | New one-shard saturating charge fold |
| total_charge | Per-shard lock and early-saturation aggregation parent |
| CopyEntryKey | New complete binary trailing-key copy |
| CreateEntry | Allocation/construction/owner transfer/key initialization parent |
| DestroyShard | New one-shard cache-reference teardown |
| Destroy | All-shard detachment then unlocked retired deletion parent |

## Inspected unchanged core definitions

| Definition | Responsibility retained |
| --- | --- |
| Entry constructor | Initialize one unpublished entry representation |
| Entry::key | Borrow the trailing binary key bytes |
| Pin constructor | Bind one existing referenced entry to its cache |
| Pin move constructor | Transfer both borrowed pointers and clear source |
| Pin move assignment | Release prior pin, then transfer both pointers |
| Pin destructor | Delegate final release through Reset |
| Pin::value | Borrow the retained opaque value |
| Pin::Reset | Release once, then canonicalize both pointers to null |
| HandleTable constructor | Allocate the original four empty buckets |
| HandleTable::PrepareInsert | Capture stable link position, replacement and growth need |
| HandleTable::Remove | Unlink one matching table entry and update element count |
| HandleTable::AllocateBuckets | Allocate and explicitly null-initialize one bucket array |
| HandleTable destructor | Release the bucket owner only; entries belong to the cache |
| Shard constructor | Initialize empty lru/in_use sentinels |
| ErasedValueDeleter::operator() | Invoke the transferred value deleter |
| ShardedLruCacheCore constructor | Initialize per-shard capacity |
| ShardedLruCacheCore destructor | Delegate complete cache-owned teardown |
| Insert | Entry ownership, visible lock, K01 admission, unlocked reclamation and Pin return |
| CanAdmit | Existing replacement-aware nonzero/overflow eligibility |
| CommitAdmission | Existing locked table/list/ref/usage/eviction transaction |
| NewId | Relaxed atomic monotonic ID allocation |
| ShardIndex | Select the top hash bits |
| DeleteEntry | Value deletion, entry destruction and unsized raw deallocation |
| Remove | Unlink one known intrusive-list entry |
| Append | Append one known detached entry at a list's MRU end |
| FinishErase | Complete one removed mapping's list/accounting/cache-reference transition |
| Retire | Push one unreachable zero-reference entry onto the retired chain |
| DeleteRetired | Iteratively reclaim the unreachable chain |
| Release | Locked Unref followed by unlocked reclamation |

These rows plus the selected ledger cover every current runtime definition in
sharded_lru_cache_core.cc and every inline runtime body in its private header.
Deleted special members are type constraints, not runtime responsibilities.
Implementation review must reconcile the final helper set; counts are design
bookkeeping, not syntax-scanner proof or production-wide completion.

## Inspected unchanged typed-wrapper definitions

| Definition | Responsibility retained |
| --- | --- |
| Handle defaulted move constructor | Move the core Pin owner |
| Handle defaulted move assignment | Move-assign the core Pin owner |
| Handle defaulted destructor | Destroy the core Pin owner |
| Handle::value | Typed const projection of the pinned opaque value |
| Handle::operator-> | Pointer syntax projection through value |
| Handle::operator* | Reference syntax projection through value |
| Handle private constructor | Move one validated core Pin into the typed owner |
| ShardedLruCache constructor | Construct the core with the public capacity |
| ShardedLruCache defaulted destructor | Destroy the core after all handles |
| Insert | Validate typed owner/charge, transfer ownership and map core Pin to Handle |
| Lookup | Map optional core Pin to optional typed Handle |
| Erase | Typed key pass-through to the core |
| total_charge | Typed accounting pass-through |
| NewId | Typed ID pass-through |
| DeleteValue | Delete one transferred concrete value |

The typed cache's deleted copy/move operations remain lifetime constraints.
Engine TableCache, block cache users and their loading policies remain separate
frontiers; this batch does not certify them.

## Independent design review

Independent source-backed review found five characterization/contract gaps.
The design now requires exact traversal-and-prepend rehash order rather than
membership-only equivalence; the public collision case identifies the
source-proven post-growth head without claiming that lookup results reveal
physical links. The allocation case now proves replacement leaves elements
unchanged by forcing the following fifth insertion to request bucket growth
before mutation.

The duplicate-pin case now creates an eviction opportunity before the last pin
release, then separately proves final-release MRU placement. Reentrant final
release explicitly targets the same shard. Finally, ordinary retired-entry
unreachability is scoped away from destructor teardown, whose stale table links
are never consulted and whose value deleters may not reenter a destructing
cache. No production edits or tests were run during this bounded design review.

## Original-source characterization

Before production edits, add four public cases:

1. A collision-chain threshold matrix inserts the two known equal-hash keys plus
   same-shard keys selected into other new-bucket indexes, replaces `Second` at
   four elements, triggers growth with a fifth mapping, then erases `Second`.
   Source inspection establishes that `Second` is the post-growth head under the
   exact traversal/prepend algorithm; public lookups do not independently expose
   physical chain order. Check every remaining value and exact indexed charge.
2. A duplicate-pin ordering case creates an eviction opportunity while one
   duplicate pin remains and proves the mapping is still indexed. It then erases
   the temporary over-capacity mapping, releases an older evictable entry before
   the final duplicate pin, and inserts once more; only that older entry may be
   evicted, proving the last release appends at the MRU end.
3. An erased last-pin reclamation case uses a value deleter that reenters Erase
   on a distinct key in the same shard; it must run only after Release unlocks,
   leave the second mapping erased, and keep its outstanding handle valid.
4. The isolated allocator fills one shard to the four-element table threshold,
   then performs an admitted replacement. It must allocate only the new
   variable-sized entry, not growth buckets, retain the old pin, publish the new
   value, and preserve exact charge. A fifth distinct insertion then permits the
   entry allocation but fails the required bucket allocation; every prior
   mapping/value/charge must remain unchanged. This proves replacement did not
   increment elements and the next new mapping still requests growth before
   publication.

Use public typed-cache operations rather than private helper tests. Keep key and
value containers outside allocation-counted regions. Do not infer arbitrary
allocator behavior from successful ordinary tests.

Build both modern_leveldb_unit_tests and
modern_leveldb_cache_allocation_tests. Freeze exact unique
ShardedLruCacheTest/CacheAllocationTest/TableCacheTest JUnit identities on
unchanged production, with no failures/errors/skips, and independently review
the new cases before production edits. Then commit handle-table, retention and
accounting/teardown topics separately, checking each relevant exact original
subset. Finish with the full frozen selection and unchanged complete
GCOVR/LCOV/NOLINT annotation multisets for both core paths and the typed header.

Independent integrated review must inspect ownership, lock visibility,
allocation-before-mutation, reentrant deletion, list membership, collision
chains, coverage-sensitive branches and every ledger row. One published exact
head must pass all hosted checks, including authoritative GCC 13 branch
coverage, allocation tests and actual Windows Debug original-name parity before
normal squash delivery.

Successful tests do not establish allocator-failure behavior outside the
isolated seams, physical copy/move/destruction counts, fairness, performance,
unsupported Pins surviving cache destruction, or whole-production completion.
