# ADR-0087: Bounded Table-Reader Reuse

- Status: Implemented
- Date: 2026-10-22

## Context

The retained read experiment bypassed 1,895,987 TableCache lookups per million
queries by keeping all 47 readers alive. Concurrent Get benefited, but that
fixture's strong pins are not a safe general file-resource policy.

Existing TableCache Find returns a move-only cache pin. Such handles protect
the mapping against capacity eviction, survive explicit Evict, and must be
destroyed before the cache. Index/data iterators and maintenance use those
contracts. Point-read reuse must not weaken them or permanently pin every
file mentioned by a Version.

This is a concrete topic under
[ADR-0085](0085-reviewed-read-optimization-adoption.md), not an adoption of the
experiment's sparse file-number pin array.

## Decision

Use bounded, per-thread weak reader hints for point Get. A hint owns no table,
file descriptor or cache pin. Locking a live weak reference creates a temporary
strong table owner for the current point lookup only.

Do not add table-reader owners to FileMetadata, Version or MANIFEST objects.
Do not introduce permanent per-file pins or a generic thread-local map that
can grow with database/file count.

### Table ownership

Select ownership once at cache construction. With reader reuse disabled, retain
the original `ShardedLruCache<Table>` unique-value backend. With reuse enabled,
cache shared Table owners through `ShardedLruCache<shared_ptr<const Table>>`.
Own the selected nonmovable backend in place in a proper typed variant; never
switch it when a shard's pressure gate closes. The generic LRU policy remains
unchanged. Identity tokens, gates and epochs are constructed only in Shared mode.

The first shared-in-both-modes implementation imposed shared-owner/control/
holder costs even with hints disabled: closing a gate does not remove them.
Default-off retains unique ownership. Its first original/control comparison
also crossed a CPU-dispatch lifetime change, so it does not isolate how much
of the measured pressure difference came from shared allocation costs.

Keep TableCache::Handle a move-only typed variant of the underlying cache pins.
Its value/arrow/dereference continue to expose the Table, not a shared-pointer
implementation detail. Find keeps the cache mapping pinned for existing
iterator/maintenance callers, preserving their eviction and lifetime behavior.
Access follows the lease's own alternative, including noexcept move assignment
between alternatives; do not infer its type from an unrelated cache pointer.

Point reads inspect the immutable backend in SearchFile. Unique mode obtains
the original typed pin through `FindUniqueForPoint` and calls Table Get
directly. Shared mode calls the complete, hint-aware TableCache Get.
The complete operation remains valid in both modes; its Unique branch and
outward Find use the same `FindUnique` helper. Outward Find wraps that pin for
its existing typed Handle interface. Shared hint processing stays out of line.
The production Unique route avoids both the outward Handle variant and the
extra complete-Get wrapper without changing ownership or the pin's lifetime.

Its hint-hit path holds only a temporary shared Table owner while calling Get.
Every fallback obtains and retains its normal mapping pin through Table Get,
including reuse-disabled and pressured paths. It may publish a weak hint
while that handle protects the owner, but does not release the cache pin
early. No shared reader is returned to an external
caller that could outlive the filesystem or comparator.

TableCache and all its existing handles still borrow the filesystem,
comparator and configured block cache under the existing obligations.
Public database operations keep their engine/services live. Weak control
blocks can survive cache destruction but cannot keep a closed table's
filesystem/comparator-dependent object alive.

### Fixed hint footprint and identity

Use one fixed-size thread-local array of 64 hints, not one array per database.
Each hint contains a complete cache-instance identity, file number, shard
generation and weak table owner. Hashing selects a slot, not identity.
Compare complete tags before weak locking.

Every TableCache gets a fresh nonempty ownership control block for an empty
identity token. Hints hold only weak tokens; compare full ownership identity
using both `owner_before` directions before locking the weak Table. An old
hint retains its control block, so a new cache cannot alias that identity,
even after object-address reuse or arbitrarily many prior cache lifetimes.
There is no wrapping global numeric identity counter.

Hashing may use the stable token object's address to select a hint position;
that address is never the identity comparison. The extra weak token and tiny
per-cache allocation remain bounded. A weak token owns no services or table.
File-number reuse across databases and concurrent caches must not alias.

Replacing a hint drops only a weak owner. Memory is bounded by the fixed array
per thread, including when a thread touches many databases. Thread exit drops
the remaining weak controls without calling a dead TableCache.

### Conservative pressure gate

Hints intentionally omit promotion while the known open population fits.
That history cannot be reconstructed merely by switching hints off.
Do not claim exact LRU victim parity at the first pressure transition.

For each of the existing 16 shards, maintain a successful-open-event count.
An open event includes a successfully constructed reader even when insertion
later fails or replaces a concurrently opened duplicate. The count is an
upper bound on distinct mappings that can have occupied the shard.

Allow hints only while this count does not exceed the shard's configured
ceiling capacity. Disable hints for that shard before inserting the first
reader that can exceed this bound. Thereafter all new point lookups in that shard use ordinary Find, retain
its pin through the data read, and perform the original recency updates.

This is deliberately conservative and monotonic for one cache lifetime.
It does not re-enable hints after deletions or try to predict a hot set.
Duplicate opens can disable hints early, which is a performance limitation,
not an identity or lifetime error. Zero-capacity shards never use hints.

While the bound holds, no capacity victim is required. At the pressure
handoff, prior skipped promotions can change the victim order: with capacity
two, A/B/hint-A/C can evict A rather than ordinary LRU's B. An already-running
hint hit can also retain its Table without retaining the cache mapping when
pressure begins. Both are intentional policy differences, not lifetime or
value differences. Measure the transition and do not enable reuse by default
if a realistic pressure workload shows a reliable harmful effect.
Pinned handles and explicit erasure remain governed by their existing rules.
The gate must use the same complete-key hash and shard-capacity rounding as
the underlying cache; do not duplicate a different shard partition.

### Explicit invalidation

Evict first erases the shared cache mapping, then advances the corresponding
shard generation with release ordering. A hint carries the generation observed
before its fallback Find and is usable only with the current generation.
Do not publish a reusable hint when the generation changes during fallback.

Generation saturates rather than wrapping. Before any contender publishes
`UINT64_MAX`, permanently close the shard's monotonic hint gate; further Evict
calls leave that generation unchanged. The successful-open CAS cannot lower
a closed count. Closing only after a wrapping increment is unsafe: another
Evict could return after recreating an ancient epoch while the first pauses.

A Get overlapping Evict may retain a reader already obtained, as existing
pins do. A later operation after Evict completes must not reuse an old hint
just because another thread still has the old table alive. Find that is
already opening may still recache after Evict, preserving its existing
documented race rather than claiming deletion synchronization.

The immutable-file ownership contract is unchanged; this feature does not
support external table mutation while a database is open.

### Options and defaults

Provide a read-reuse switch for control measurements and operational opt-out.
Keep it disabled by default. The final matched resident/concurrent benefit
supports a workload-dependent opt-in, not an unconditional default.
Constrained-table-cache pressure still exposes additional work after hints
stop: the backend remains Shared, not Unique. Matched disabled results are
mixed and do not establish an overhead-free default path. The final admission
below explicitly accepts that residual performance risk; it does not report
the earlier preservation goal as achieved. Do not extrapolate the strong-pin
prototype or attribute the older CPU-selection-confounded costs to ownership
alone.

Retain the original shared-in-both-modes campaign as its distinct ownership
baseline. A measurement-only SharedNoHints object may keep constructor-true
ownership/counting behavior and disable only hint use; it is not a production
option or a replacement for ordinary off/on measurements.

Do not change max_open_files, silently add an independent persistent-pin
budget, or retain a table merely because its FileMetadata is still reachable.
Ordinary active reads can retain files transiently, as existing handles can;
weak hints add no persistent open-file owners.

## Required evidence

- Existing Find/Handle behavior, pinned replacement under pressure, cached
  size ignoring, errors, noncached failures and destruction ordering.
- Run the Find/Handle contracts in both immutable backend modes, including
  cross-alternative noexcept move assignment and default-off allocation parity.
- Same-thread hint reuse and independent multi-thread hints, with independent
  output buffers and identical values.
- Explicit Evict with an old live handle and a same-number replacement table.
- Controlled Get/Evict/fallback-publication interleavings; no stale hint use
  after completed invalidation.
- Distinct databases/cache instances with identical file numbers and reused
  object addresses.
- Many database lifetimes on one thread without growing hint storage.
- Closing a cache does not leave files open through hints; thread exit does
  not access destroyed cache/filesystem/comparator state.
- Exact shard-capacity boundaries, collisions, zero capacity, duplicate
  opens, insertion exceptions and monotonic fallback after pressure.
- A/B/hint-A/C exact-victim characterization of the intentional handoff
  difference, followed by ordinary fallback promotions.
- A paused fallback Get must keep its mapping pinned while another thread
  inserts competitors. Separately pause a hint-hit read across the pressure
  transition: its shared owner protects values/files without pretending to
  be the original mapping pin.
- Barrier-controlled duplicate/maintenance opens at the exact boundary,
  including owner-wrapper/insertion exceptions. Count successful construction
  centrally before either later allocation can throw, and saturate at
  capacity-plus-one without an overflowing ceil expression.
- Evict paused between erase/generation advancement; fallback paused between
  obtaining its handle and hint publication; and an old open completing after
  Evict. Retain the documented allowed recache race but reject direct reuse
  of an old-generation hint.
- Near-exhaustion and two-Evict overtake schedules: generation never repeats
  and a terminal generation implies a closed gate. Acquire-load generation
  before inspecting the gate, rather than sampling an earlier gate value.
- A pressure corpus measuring handoff and disabled variants, not merely an
  all-resident hit test or a claim that disabling hints restores past history.
- Baseline/candidate CPU-selection code and lifetime must match. Hot Get-body
  identity alone is insufficient for a workload that can open readers.
- Count actual successful random-access table opens during timing. A file
  population above a shard target establishes potential pressure, not observed
  eviction/reopening. Verify the corpus in an independent owned database copy,
  checking distinct directory and per-SST file identities against both copies
  and the sealed source. Then warm the freshly opened measured copy with point
  Get only; an iterator's
  temporary pins must not establish an uncontrolled over-target starting cache.
  Both copies remain isolated from the immutable input image and from each
  other, including maintenance run during verification-copy shutdown.
- One/four-reader Get measurements with hints disabled/enabled and original
  ownership baseline, plus read/fd lifetime counters and negative outcomes.
- Publish a passed verification verdict only after inherited and all matched
  source/isolation/open checks succeed. Keep provisional inherited reports in
  memory, clear stale derived verdicts on rerun, and atomically publish the
  final complete report; a later failure must not leave an earlier success.

## Review boundary

Obtain independent architecture review before implementation. An independent
implementation persona must review identity, weak/strong ownership,
invalidation races and pressure gating before publication.

Do not claim that a weak hint has the same cost or hit rate as the strong-pin
fixture. Update table-cache contracts, read-path architecture and API options
with the final measured behavior.

Independent design review found and resolved two blocking contract errors:
fallback now retains its real mapping pin through the read, and the pressure
handoff explicitly admits lost promotion history rather than claiming exact
LRU parity. Re-review approved this concrete design, not default-on adoption
or implementation correctness.

Independent pre-implementation re-review also approved weak control-block
instance identity and the corrected terminal-generation ordering. Its
two-Evict counterexample was resolved by closing the gate before terminal
publication and never wrapping an epoch.

Independent design re-review approved immutable typed ownership selection after
the first 192-process campaign exposed the default-off cost. Unique mode does
not pay shared control/holder/token allocations or hint-state maintenance.
This restores ownership semantics, not a presumed performance result; variant
dispatch and layout still require fresh original/off/on controls.

The second 192-process campaign retained all observations. Unique ownership
removed the mandatory shared allocations; its constrained-open-file serial
cases were 5.38% and 4.52% slower than the frozen original, with no faster pair
in either six-round cell. This is a cumulative observation, not isolated proof
of a default-off reader-feature regression: the original `51c000a` cached
RuntimeProbe, while the candidate inherited per-reader CPU detection from
`05033d5`. See the measured correction in [ADR-0086](0086-persisted-full-table-filters.md#admission-time-cpu-selection-correction).

The fixture has 47 tables, with six in one shard whose ceiling is four.
That establishes potential localized pressure; the old harness did not count
timed opens. New controls must prove actual pressure and exclude uncontrolled
iterator-warmed soft-target overshoot before making an admission claim.

An independently reviewed draft isolates the added out-of-line point Get and
outward typed Handle by using a direct Unique pin. It remains an unadopted
experiment, not this implementation's required representation. Do not ship it
merely to compensate for the mismatched control. First run original/off/on
with the same fixed selector and measured open events. Retain both earlier
campaigns and the draft.

An exact MSVC layout probe found a 16-byte original pin and a 24-byte outward
variant pin, but both Result representations remain 72 bytes because Error is
64 bytes. Do not attribute the slowdown to a larger Result return buffer.
Any later routing experiment still requires linked-production-path inspection,
implementation review and fresh matched controls.

### Matched control result and bounded routing follow-up

The corrected v2 campaign completed and independently verified all 192
processes against accepted `02ed212`, with identical filter/CPU-dispatch source,
separate byte-identical nonaliased verification/measurement copies, zero
resident timed opens, and positive pressure opens in every accepted process.
The original zero-timing matched fixture is retained: independent review found
its inherited verifier published success before matched checks. Two observed
RED regression cases preceded the transactional v2 verifier fix.

These are paired median wall-time improvements versus the matched original;
positive is faster. No process or round was discarded.

| max_open_files | Logical cache | Readers | Disabled | Enabled |
|---:|---:|---:|---:|---:|
| 1000 | 8 MiB | 1 | -0.95% | -0.01% |
| 1000 | 8 MiB | 4 | -3.30% | +7.75% |
| 1000 | 512 MiB | 1 | -1.13% | +2.86% |
| 1000 | 512 MiB | 4 | -2.36% | +11.48% |
| 74 | 8 MiB | 1 | +0.92% | -0.03% |
| 74 | 8 MiB | 4 | -1.18% | -0.85% |
| 74 | 512 MiB | 1 | -1.48% | -1.04% |
| 74 | 512 MiB | 4 | -0.02% | +0.42% |

The earlier roughly five-percent serial-pressure gap is no longer present,
but the disabled resident four-reader path is slower in five of six pairs in
both cache cells. Enabled resident four-reader reads beat the original in all
six pairs, and beat the candidate disabled arm by 12.06% and 12.86%.
This is valuable opt-in evidence, not permission to silently ship default-off
overhead or claim that its cause is already proven.

Reactivate the independently reviewed direct-Unique-pin candidate only now,
after matched controls established a remaining disabled-path cost. Preserve v2 and
all older observations; keep the Shared protocol and default false unchanged.
The inline Unique path still requires linked production-routing proof, fresh
contracts, independent implementation review and another complete matched
campaign. No default-preservation claim follows from the source refactor alone.

Optimized MSVC inspection does not claim that `inline` forced inlining:
SearchFile and PointLookup::Visit still call TableCache::Get. Its emitted body
falls from 1,145 to 462 bytes, however, and the Unique branch now calls the
original typed `FindUnique` pin and Table Get without weak-hint state or
outward Handle visitation. Require the linked image to match that inspected
route and measure its actual effect rather than inferring a gain from size.

### Typed-route v3 result and direct disabled-path candidate

The complete v3 campaign independently verified all 192 processes and the
actual linked typed route. Enabled resident four-reader results beat the
matched original by 7.55% and 10.84%, in all six pairs for each budget.
The disabled resident path still costs 1.56% at 8 MiB and 1.11% at 512 MiB,
with five slower pairs in each cell. Other disabled cells range from -0.73%
to +1.55%. Do not relabel that remaining result as measured preservation.

The next bounded design restores the original disabled production call graph:
SearchFile obtains the original typed pin and calls Table Get directly, while
the Shared branch keeps the complete hint-aware Get. Expose only an immutable
ownership-mode observation and `FindUniqueForPoint` on the private engine
interface. That typed method requires Unique mode, established by SearchFile
before entry, and delegates to the same lookup/open/insert helper as outward
Find. Keep complete Get valid for its existing direct callers in both modes.
No ownership flag, pointer reinterpretation, hint/gate change, force-inline
macro or public option is added.

Use the existing TableLookupKind as the private source-decision type instead
of converting every table result through an equivalent second enum. Memory
lookup constructs only the same Missing/Value/Deletion alternatives; Table Get
already returns this checked internal type. This removes redundant result
conversion without weakening persistent-input or I/O validation.

Require actual linked evidence that the disabled SearchFile branch calls the
typed Find and Table Get without the added complete-Get wrapper. Keep all v3
observations, verify every lifetime/error/seek/tombstone contract, obtain
independent design/implementation reviews, and run fresh matched controls
before admission. This is another measured hypothesis, not a claimed recovery
or a preset percentage target.
Independent architecture review approved this bounded design. The ownership
observer must not report per-shard hint eligibility: Shared mode remains Shared
after pressure closes a gate. The typed method's cache/comparator/filesystem
borrows retain the existing Find lifetime obligations.

### Final direct-route v4 evidence and admission

The sealed `reader-production-direct-v4` campaign independently verified all
192 fresh processes for source `6d75752f70f0e7f1a279ee64165a381767a9f882`
against accepted `02ed212`. Every resident process had zero timed table opens;
every pressure process had positive opens. Original and candidate filter and
CPU-selection sources matched. Verification used separate nonaliased copies,
and the final verdict was published only after all inherited and matched
checks. Actual linked instructions establish the Unique branch's direct
typed Find/Table Get route; there is no forced-inlining claim.

These are paired median wall-time improvements against the original, with
all six rounds retained. Positive is faster. The counts are faster pairs
out of six, not an acceptance threshold.

| max_open_files | Logical cache | Readers | Disabled | Faster pairs | Enabled | Faster pairs |
|---:|---:|---:|---:|---:|---:|---:|
| 1000 | 8 MiB | 1 | +3.48% | 4/6 | +5.00% | 5/6 |
| 1000 | 8 MiB | 4 | +0.34% | 4/6 | +8.55% | 6/6 |
| 1000 | 512 MiB | 1 | +2.28% | 5/6 | +4.97% | 6/6 |
| 1000 | 512 MiB | 4 | -1.92% | 2/6 | +10.09% | 6/6 |
| 74 | 8 MiB | 1 | -0.91% | 1/6 | -0.26% | 2/6 |
| 74 | 8 MiB | 4 | +0.63% | 4/6 | +0.93% | 4/6 |
| 74 | 512 MiB | 1 | -0.51% | 3/6 | -0.11% | 2/6 |
| 74 | 512 MiB | 4 | -0.57% | 2/6 | -3.17% | 0/6 |

Enabled resident four-reader results beat the disabled candidate by 7.88%
and 10.88%, in all six pairs for both budgets. The pressured 512 MiB,
four-reader enabled case is slower than the original in every pair, and
2.34% slower than the disabled candidate with only one faster pair.
Gate closure does not remove Shared ownership/allocation costs or recover
the previously skipped promotion history; these observations do not isolate
one causal cost.

The separate predeclared 36-process resident four-reader screen is retained,
not pooled with the complete campaign. Disabled results were -0.65% and
+0.61% against the original, with two and three faster pairs respectively;
enabled results were +9.24% and +14.18%, each with six faster pairs.
Neither this screen nor the mixed complete disabled results proves
neutrality. In particular, retain the complete campaign's -1.92% disabled
resident result rather than dismissing it as noise.

Independent architecture review approves bounded opt-in admission as this
explicit tradeoff, not a default-preservation verdict. Independent
implementation review retains approval for the exact `6d75752` changes and
found no additional implementation blocker. The source passed 1,054 unit
cases, 19 extended cases, strict GCC checks and ordinary cross-platform,
coverage and sanitizer CI. Earlier campaigns, rejected verifier drafts and
all adverse results remain distinct evidence.

Admit the option with default `false`. Accept the documented residual
default-path performance risk in exchange for the repeated concurrent-resident
benefit and reviewed ownership/invalidation contracts. Do not enable it
automatically, promise general acceleration, or claim disabling reuse erases
every feature-related cost.
