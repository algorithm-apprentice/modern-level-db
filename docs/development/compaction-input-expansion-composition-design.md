# Bounded compaction input expansion

## Status and scope

Accepted design for C05 from the
[production composition plan](production-function-composition-plan.md).
Design-only #195 was independently reviewed and merged after all 20 exact-head
checks succeeded. Its bounded implementation record follows below.
Only SetupInputs in `src/engine/compaction_picker.cc` and its private expansion
children/direct tests are in scope. No scoring, L0 closure, iterator/output,
native filesystem or benchmark changes.
The output/history implementation #194 changes none of these source/test/design
paths. This independent design adds no shared-ledger update.

## Existing contract

SetupInputs closes source boundary files, computes the source range, collects
and closes next-level boundary files and widens the union range. It attempts
source expansion only when next-level inputs are nonempty.

Expansion collects source overlaps over the union, closes their boundary files,
and proceeds only if source count grows and combined expanded-source/next-level
bytes are strictly below 25 times target size. Count failure short-circuits
both byte sums. Sum/threshold failure suppresses expanded next-level collection.
Expanded next-level overlaps also receive boundary closure; expansion is accepted
only if their count equals the original next-level count.

On acceptance, source range, source/next input vectors and union range all change
together. Otherwise they remain unchanged. Grandparents are selected only after
this decision over the final union; compact pointer copies the final source
range's largest internal key, not the union's largest. The version remains live
until both borrowed range endpoints are consumed and transferred ownership is
published in the returned Compaction.

## Named private composition

Introduce private named `InputExpansion` aggregate containing:

- Owned expanded source and next-level vectors of original Version::File owners.
- Borrowed expanded source KeyRange.

`TryExpandInputs` borrows version, comparator and existing source/next spans,
level, target size and union range. It returns optional<InputExpansion>.
Return nullopt for no next inputs, no source count growth, rejected byte budget
or changed next-level count. These are ordinary optional decisions, not errors
or silently swallowed exceptions.

Compose existing OverlappingFiles, AddBoundaryInputs, TotalFileSize, RangeOf and
Widen in their original order. Keep the count and strict work-bound test as one
short-circuited admission condition; do not replace with eager byte computation.
Retain original next-size sum as the left operand and expanded-size sum as the
right operand, arithmetic/overflow policy and comparator order. This does not
claim evaluation sequencing for the operands of C++ addition.
Do not compare file numbers or substitute bytewise range checks.

Construct the named result only after expanded next count matches. The parent
computes its union by the same range assignment and Widen operation after
replacing input vectors. Moves do not introduce allocations or shorten
metadata lifetime. Returning rejection destroys candidate vectors without
touching caller-selected input state.

SetupInputs composes original two-level closure, optional expansion application,
original grandparent collection and named returned Compaction construction.
Apply the result's source range and move its source/next vectors together, then
assign/widen union. Original version ownership stays in the parent until the final
aggregate. No ref-parameter forest, new public type, allocated context or generic
planner framework.

| Definition | Responsibility and recursive boundary |
|---|---|
| `TryExpandInputs` | Compose one speculative bounded expansion using existing overlap, boundary, size and range primitives. |
| `SetupInputs` | Compose original closure, optional admitted expansion and final grandparents/owned compaction result. |

No public signature changes. Other picker parents/helpers/lambdas remain
subsequent recursive frontiers, not implicitly certified by this batch.
The private aggregate carries state needed to apply a complete admission,
not configurable planning policy.

## Characterization before extraction

Existing picker tests already check equality rejection and one-byte-below
acceptance at the 25-fold byte threshold, changed next-input count rejection,
boundary chaining, L0 transitive closure, reverse comparator, grandparents
and source-range compact pointers.

Strengthen acceptance/rejection with different final grandparent sets and exact
source/next file identities. A accepted expansion's source endpoint must change
the pointer; rejected expansion must retain the original endpoint and grandparent
union. Include a next-level boundary chain in expanded-next admission so omitting
its AddBoundaryInputs cannot accidentally pass by plain overlap count.

Retain returned compaction after the caller's version variable is released and
validate source pointers, owned compact pointer and grandparent bounds. Directly
use PickCompaction, not the new private helper. Track a uniquely named expanded
next-level bound only if needed for the byte-budget short-circuit oracle;
avoid attributing ordinary score or original closure comparisons to expansion.

Run CompactionPickerTest/CompactionPickerReverseTest/CompactionTest before/after
with exact unique JUnit case-set equality, added names present and no
failures/errors/skips. Use actual modern_leveldb_unit_tests target, existing
format/documentation/whitespace checks, unchanged authoritative exact-head
remote CI/coverage. No allocation injection or altered thresholds/exclusions.

## Review and delivery

Independent design review precedes accepted merge. Characterization precedes
production extraction. A bounded implementation and combined independent review
follow; related internal commits share one implementation PR.
This batch may run independently of #194 with no shared file edits.
The full production recursive audit and remaining frontiers still apply.

Independent design review found no significant issue. It clarified addition
operand placement versus unspecified evaluation sequencing, now stated above.
Expanded-next boundary and short-circuit oracles remain implementation-review
requirements. The reviewer inspected source/tests without executing tests.

## Implementation record

Three new characterization cases passed and were committed before production
extraction: accepted/rejected work budget changes grandparent sets and source
pointer while preserving original metadata identity/ownership; expanded-next
admission retains its boundary chain; tracked comparator pair proves byte-budget
rejection skips expanded-next selection, and an admitted budget then rejects a
larger next-input set.

The implementation simplifies the accepted proposal's result by omitting its
union-range field. Compute final Widen in SetupInputs after replacing source and
next vectors, exactly as original code. Computing it inside TryExpandInputs
would run configured comparator calls while the old next-input vector still
held additional metadata owners; this refinement preserves original owner
replacement/compare order and removes unneeded carried state. It is explicitly
included in combined implementation review.

TryExpandInputs composes original optional guards and candidate primitives;
SetupInputs composes its original closure, admitted vector/range replacement,
Widen, grandparents and named returned compaction. These two ledger definitions
are implemented; remaining picker helpers/parents/lambdas are still recursive
frontiers.

All 29 selected picker/reverse-picker/tracking/compaction cases pass before/after
with exact unique JUnit parity, all three added names present and no
failures/errors/skips. Each run rebuilt actual modern_leveldb_unit_tests in
Windows Debug on original and final C05 source. #194's new compaction tests
were not yet part of this independent branch, so its integrated case set will
be separately validated once available. The shared ledger remains untouched
while #194 owns its delivery updates.
Independent combined implementation review found no significant issue and
confirmed the smaller result preserves original replacement-before-Widen
ordering. It independently inspected both 29-case XML sets without running
tests. Exact-head remote CI remains the delivery gate.
