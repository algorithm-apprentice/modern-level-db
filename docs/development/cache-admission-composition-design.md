# Cache admission and locked insertion composition

## Status and scope

Design proposal for K01 from the
[production composition plan](production-function-composition-plan.md).
Scope is ShardedLruCacheCore::Insert's admission/commit children and private
declarations. Public typed cache validation, handle table growth/rehash,
retention lists, eviction/release and allocation strategy are not redesigned.
No benchmarks or new coverage exclusions.
This design changes only its own document, independent of #196/#198.

## Original transaction

Insert takes transferred value ownership before entry allocation, materializes
one variable-sized entry with copied key bytes, chooses its shard, and locks.
It always calls PrepareInsert, computes replacement-adjusted base charge and
overflow admission before checking nonzero shard capacity.

Admission is `capacity != 0 && entry.charge <= max_size - base_charge`;
base charge subtracts only the existing mapping's charge, not old externally
pinned values or all physical retention. Overflow/zero-capacity rejection still
returns the new uncached external pin without replacing any mapping.

Admitted insertion calls potentially allocating CommitInsert before any
reference/list/usage mutation. It then assigns two refs, marks cache ownership,
appends in_use, erases the replaced entry, adds new usage and evicts in that order.
Retired value destruction occurs after unlocking. Retired entries are already
unreachable from the hash table; deleters may reenter and erase the just-inserted
mapping while its initial external reference keeps the returned value alive.
Only after retired deletion does Insert release its PendingEntry into Pin.

If entry or bucket growth allocation throws, transferred owners unwind without
index/list/accounting publication. PendingEntry is outside the lock block, so
its value deleter runs after the shard mutex unwinds. No allocation recovery,
catch, fallback mapping or translated Status is added.

## Named private children

`CanAdmit(const Shard&, const Entry&, PreparedInsert)` computes the original
replacement-adjusted base charge and charge_fits, then returns capacity policy.
Keep both arithmetic calculations before the capacity conjunction; do not
skip PrepareInsert or change overflow arithmetic for zero capacity.
It is noexcept, borrowing only proven-valid internal state.

`CommitAdmission(Shard&, Entry&, PreparedInsert, Entry*& retired)` composes
CommitInsert, reference/cache ownership, list admission, FinishErase, usage
addition and Evict exactly as original. It is not noexcept: bucket growth may
throw before commit. It does not lock, allocate another transaction object,
release the pending entry, or call DeleteRetired.

Insert composes owner acquisition, shard/retirement setup, visibly scoped mutex,
preparation/admission/commit, unlocked retirement and returned pin transfer.
Keep mutex ownership visible in the parent. PreparedInsert's bucket position
is consumed under the same mutex with no intervening table mutation.
Do not duplicate existing cache retention operations into new helpers.

| Definition | Domain composition or stopping boundary |
|---|---|
| `CanAdmit` | Replacement-aware accounting eligibility, separate from allocation/commit. |
| `CommitAdmission` | One locked cache admission transaction using existing index/list/accounting primitives. |
| `Insert` | Compose allocation/ownership, explicit locked transaction and unlocked reclamation/pin return. |

Remaining HandleTable, list/ref/eviction helpers and typed/public cache paths are
subsequent recursive frontiers. This is not whole-cache certification.

## Characterization before extraction

Existing tests cover binary/empty keys, public invalid value/charge rejection,
pinned replacement, growth/collisions, LRU, pinned overcapacity, zero capacity,
charge overflow/saturation, concurrent operations and reentrant eviction.
Existing isolated allocation tests count entry/lookup allocations and inject
entry/bucket failures with mapping/accounting and transferred-value evidence.

Add replacement overflow rejection plus exact-max admitted replacement in the
presence of another pinned huge-charge entry. Assert old mapping preserved on
rejection, new uncached value remains owned, admitted replacement's old external
pin remains valid, and exact accounting without overflow.
After erasing one admitted mapping, assert the remaining exact charge; a
saturated aggregate max alone cannot prove correct replacement subtraction.
Add replacement-specific reentrant deleter erasing the newly indexed mapping;
the returned new pin must survive and accounting/index become empty.

Add overflow rejection when prepared insertion would otherwise grow buckets.
The isolated allocator should see only one entry allocation, no bucket allocation
or exception, while all original indexed handles/mappings/accounting survive.
Explicitly assert no allocation exception as well as one counted successful
allocation, since the existing counter counts successful allocations.
Prepare key/value containers outside the counted session and use real public
Insert, not private helper tests or allocation exemptions.

Build both actual targets modern_leveldb_unit_tests and
modern_leveldb_cache_allocation_tests (separate executable), then run
ShardedLruCacheTest/CacheAllocationTest and directly coupled TableCacheTest.
Record exact unique JUnit before/after sets, added names and no
failures/errors/skips. Preserve existing formatting/docs/whitespace checks and
authoritative exact-head remote CI/coverage, including allocation-failure jobs.
Tests measure fixture-visible reentrancy and actual isolated allocator events,
not arbitrary native allocation failures.

## Review and delivery

Independent design review precedes accepted merge. Characterization precedes
production extraction and independent combined review before publication.
Normal merge requires all applicable exact-head successful checks.
No shared-ledger edits while another pending implementation owns that file.
Whole-production recursive completion still requires remaining frontier work
and both-native-branch definition/lambda audit.

Independent design review found no significant issue. Its oracle cautions about
saturating aggregate totals and successful-only allocation counts are included
above. The reviewer inspected source/tests/target declarations without executing
builds or tests.
