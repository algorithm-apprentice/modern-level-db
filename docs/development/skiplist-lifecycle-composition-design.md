# SkipList Node, Publication and Search Composition

## Scope and delivery

Design-first private-header follow-up to the production function composition
plan and expanded production scan.
Source baseline: `de1ed34ad8c336f3cddc9efb21f7f0ce560f506c`.
The entire arena-backed SkipList template was absent from the original
implementation-file-only inventory; the current expanded scan exposes
39 existing syntax-level definitions in src/memory/skiplist.h.

This larger related batch gives every existing SkipList definition a bounded
semantic disposition while extracting node-layout initialization, insertion
publication, shared reverse/last search, and iterator sentinel normalization.
Keep reviewed design, original-source characterization and responsibility-sized
implementation commits locally, then publish one non-draft PR.
The design and original characterization are accepted; both production topics
are implemented locally. Integrated review is complete; delivery remains
pending.

Production changes are limited to src/memory/skiplist.h. Arena and MemTable
production are real owner/caller context but remain unchanged. Tests stay in
skiplist_test.cc, with ArenaTest, MemTableTest, LookupTest and DatabaseTest as
coupled public behavior. No public API, entry concept, node bytes, random-height
sequence, comparator policy, writer serialization, atomic memory order,
allocation/accounting, benchmark, runtime assert, diagnostic conditional,
coverage exclusion or threshold change.

## Existing lifetime and concurrency contract

The SkipList borrows its comparator and Arena. Entries and their referenced
storage remain immutable after publication; every node lives until Arena
teardown. One externally serialized writer may insert while concurrent readers
traverse. Atomic links publish nodes; they do not serialize writers.

NewNode must allocate one fixed Node followed by `height` atomic links in one
Arena-aligned region, construct the immutable entry, then construct every link
as null before any publication. No individual destructor runs. The existing
Entry and Link trivial-destruction/alignment constraints remain the storage
proof; no runtime guard is added.

Insert first searches every existing level and retains predecessor pointers.
It chooses the deterministic random height, initializes newly exposed
predecessors to the head, allocates/initializes the complete node, may publish
the larger max-height hint, then links levels from zero upward. A reader may see
the larger height before the first link but observes the already-initialized
null head link and descends. At each level the node's outgoing relaxed link is
initialized before a release store makes the node reachable; reader Next uses
acquire. Preserve this exact order and memory orders.

## Topic one: trailing-link layout and complete node initialization

Add NodeStorageSize(height), the fixed/aligned node prefix plus all trailing link
bytes. Add InitializeLinks(storage, height), constructing null Links in
ascending level order from the original Arena-returned std::byte pointer.
Construction address arithmetic must remain rooted in that live backing byte
array, exactly as in the baseline, rather than being recovered from Node.

NewNode retains parent Arena allocation, immutable Node construction, complete
link initialization and returned stable pointer. Allocation or Node
construction failure occurs before link construction/publication; link
construction is the existing trivial atomic initialization. No alternate owner,
per-node delete, padding change or zero-height path.

Node::LinkAt remains the original post-construction access boundary and launders
the already-constructed trailing Link. This batch does not generalize its
existing node-relative representation access into pre-construction code or
claim a new strict-provenance guarantee for the baseline tail-allocation idiom.

## Topic two: insertion height and per-level publication

Add CurrentHeight(), the shared relaxed max_height load used by insertion and
both search parents. The stored value remains in [1, MaxHeight] under the
single-writer invariant.

Add PreparePredecessorsForHeight(predecessors, current_height, height), filling
only newly exposed levels with head. Existing levels retain the exact search
result. Add PublishHeightIfNeeded(height, current_height), keeping the relaxed
store and the original before-first-link position.

Add LinkNodeAtLevel(node, predecessor, level), one publication transaction:
read predecessor's current successor relaxed, set the new node's outgoing link
relaxed, then release-store the node through the predecessor. It adds no
comparison, allocation, retry or fence.

InsertAfterSearch retains random-height selection, current-height sampling,
predecessor preparation, complete node creation, height publication and
ascending per-level publication in that order. Insert retains its predecessor
array, lower-bound search and delegation. Duplicate insertion remains a caller
error rather than a recoverable branch.

## Topic three: reverse/last search and iterator sentinel normalization

FindGreaterOrEqual retains the head start, current top-level selection,
acquire-link traversal, EntryIsAfterNode comparison, optional predecessor
capture and level descent. It uses CurrentHeight but does not share a generic
callback/search framework with reverse traversal.

Add ReachedUpperBound(next, upper_bound): null successor always stops; an absent
upper bound never compares and means keep advancing; otherwise stop when the
next entry is greater than or comparator-equal to the strict upper bound.
Comparator evaluation count/order therefore remains absent for FindLast and
unchanged for FindLessThan.

Add FindLastBefore(optional upper_bound), the shared descending search used by
FindLessThan and FindLast. It starts at head/current top level, advances while
the next node is admitted, descends when the bound/end is reached, and returns
head when no real predecessor exists. FindLessThan passes the entry address;
FindLast passes null.

Add Iterator::SetPosition(node), mapping the private head sentinel to the public
invalid null position. Prev and SeekToLast delegate their search result through
it. Seek/SeekToFirst/Next retain direct positions because their search/link
results never return head. No validity guard is added below the existing caller
precondition for entry/Next/Prev.

## Selected sixteen-definition ledger

| Definition | Composition or stopping boundary |
| --- | --- |
| Iterator::SetPosition | New head-sentinel to public-null normalization |
| Iterator::Prev | Strict predecessor search then normalized publication |
| Iterator::SeekToLast | Last-node search then normalized publication |
| NodeStorageSize | New fixed node plus height-dependent links size leaf |
| InitializeLinks | New complete ascending null-link construction rooted in Arena storage |
| NewNode | Arena allocation, immutable node construction and link initialization |
| CurrentHeight | New shared relaxed height snapshot |
| PreparePredecessorsForHeight | New head initialization for exposed levels |
| PublishHeightIfNeeded | New relaxed search-hint publication |
| LinkNodeAtLevel | New outgoing-link initialization and release publication |
| InsertAfterSearch | Height/predecessor/node/publication orchestration |
| FindGreaterOrEqual | Existing lower-bound/predecessor search using CurrentHeight |
| ReachedUpperBound | New end/optional strict-upper-bound decision leaf |
| FindLastBefore | New shared reverse/last descending search |
| FindLessThan | Strict-upper-bound adapter |
| FindLast | Unbounded-last adapter without comparator calls |

## Thirty-two inspected unchanged definitions

| Definition | Responsibility retained |
| --- | --- |
| SkipList constructor | Borrow comparator/Arena and create the full-height head |
| deleted comparator-rvalue constructor | Prevent borrowing a temporary comparator |
| deleted const-comparator-rvalue constructor | Prevent borrowing a const temporary comparator |
| deleted copy constructor | Preserve unique list/Arena relationship |
| deleted copy assignment | Preserve unique list/Arena relationship |
| deleted move constructor | Keep node/comparator/Arena addresses stable |
| deleted move assignment | Keep node/comparator/Arena addresses stable |
| defaulted SkipList destructor | Let Arena own all node reclamation |
| Insert | Capture predecessors then publish one unique entry |
| Contains | Lower-bound search then comparator equality |
| Iterator constructor | Borrow one stable SkipList |
| deleted Iterator copy constructor | Preserve one cursor owner |
| deleted Iterator copy assignment | Preserve one cursor owner |
| deleted Iterator move constructor | Keep cursor identity/address simple |
| deleted Iterator move assignment | Keep cursor identity/address simple |
| defaulted Iterator destructor | Release no storage |
| Iterator::valid | Test public null position |
| Iterator::entry | Borrow current immutable entry under valid precondition |
| Iterator::Next | Follow level-zero acquire link |
| Iterator::Seek | Publish lower-bound result |
| Iterator::SeekToFirst | Publish head level-zero successor |
| Node constructor | Initialize one immutable entry |
| Node::Next | Acquire-load one published link |
| Node::NextRelaxed | Writer-private relaxed-load one predecessor link |
| Node::SetNext | Release-store one reader-visible link |
| Node::SetNextRelaxed | Initialize one unpublished outgoing link |
| Node::LinkAt | Original laundered projection of one constructed trailing Link |
| LinksOffset | Align trailing Links after fixed Node |
| NextRandom | One deterministic minimal-standard seed transform |
| RandomHeight | Bounded geometric height selection |
| Equal | Comparator-defined equality leaf |
| EntryIsAfterNode | Null-safe strict node-before-entry decision |

The 16 selected/new definitions plus these 32 rows yield 48 post-change
definitions and cover every current syntax-level SkipList function, including
deleted/defaulted type constraints. Counts are bounded ledger bookkeeping, not
compiler-resolved call-graph proof or production-wide completion.

## Original-source characterization

Existing cases already prove empty/sentinel behavior, unique ordered insertion,
exact LevelDB arena block consumption at key 216/217, forward/reverse seek
boundaries, randomized std::set parity, binary borrowed entries, and
single-writer/concurrent-reader publication of initialized ordered nodes.

Before production edits, add four public/internal-operation cases:

1. A recording comparator inserts three keys, clears its trace, and proves
   SeekToLast performs zero comparisons. Prev uses only the current strict upper
   bound, reaches the predecessor, and normalizes head to invalid from the first
   entry.
2. An iterator remains on its immutable current node while later insertions add
   neighbors before/after it; subsequent Next/Prev observe the newly published
   level-zero links without losing or mutating the current entry.
3. A comparator that groups integers by decade proves Contains and Seek use
   comparator equality/order rather than operator equality, without inserting
   duplicate comparator-equivalent entries.
4. One byte is allocated before SkipList construction to force aligned node
   allocation from an unaligned Arena cursor; mixed-order insert/search/
   iteration must remain ordered and preserve the existing Arena accounting.

Use the existing internal SkipList API, not private helper exposure. Literal
expected entries/comparator observations must be independent of production
helpers. Do not claim physical tower heights are publicly observable beyond the
existing deterministic arena-consumption and concurrency evidence.

Freshly build modern_leveldb_unit_tests on unchanged production and freeze exact
unique identities for SkipListTest, ArenaTest, MemTableTest, LookupTest and
DatabaseTest, with no failure/error/skip. Independently review new cases before
production edits. Commit node layout/publication and reverse search/iterator
topics separately, checking exact relevant original subsets. Finish with full
frozen identity parity and complete GCOVR/LCOV/NOLINT annotation multiset
equality for skiplist.h.

Local GCC Release warnings-as-errors should compile SkipList, MemTable and the
selected tests with `-fno-range-for-ext-temps` where supported to model GCC 13
range lifetime. Independent integrated review must reconcile all 48 ledger rows,
memory orders, layout calculations, comparator counts and iterator lifetimes.
One published exact head must pass every hosted check, authoritative GCC 13
branch coverage and actual Windows Debug original-name parity before normal
squash delivery.

Successful tests do not establish unsupported duplicate insertion, concurrent
writers, comparator thread safety, allocator failure, physical tower shape,
lock freedom/fairness, performance, entries outliving Arena, or whole-production
completion.

## Local implementation evidence

The accepted design is commit `12738b1`. Original-source characterization is
commit `729af5b`; independent bounded review found no significant issue and
syntax-checked the new cases with GCC 16.1 Release warnings-as-errors plus
`-fno-range-for-ext-temps`.

A fresh unchanged-production Windows Debug build froze 100 unique identities:

| Suite | Cases |
| --- | ---: |
| SkipListTest | 11 |
| ArenaTest | 8 |
| MemTableTest | 13 |
| LookupTest | 14 |
| DatabaseTest | 54 |

The production topics remain separate local commits. `d498163` composes
trailing-link layout, complete node initialization, height preparation and
release publication; its exact original SkipList/Arena/MemTable subset contains
32 identities. `65b93f6` composes shared reverse/last search and iterator
sentinel normalization; the final integrated selection exactly matches all
original 100 identities, with no failure, error, skip or duplicate.

The complete GCOVR/LCOV/NOLINT annotation multiset in skiplist.h is unchanged.
A post-change expanded scan before integrated-review correction reported
49 SkipList definitions. Integrated review found that the new shared
node-relative LinkAddress extended the baseline tail-access idiom into
pre-construction pointer arithmetic. The correction removes that helper,
roots InitializeLinks in the original Arena storage pointer, restores
Node::LinkAt unchanged, and requires a corrected final scan of 48 definitions,
zero lambdas and zero recovery nodes, matching the 16 selected/new plus
32 unchanged ledger. Local GCC Release warnings-as-errors with
`-fno-range-for-ext-temps` compiles MemTable, SkipList/MemTable/Lookup tests and
all DatabaseTest translation units. Documentation validation and `git diff
--check` pass. Hosted GCC 13 branch coverage, sanitizer/runtime jobs, actual
Windows Debug identity parity and normal merge remain delivery obligations.

Initial integrated review found the pre-construction provenance regression
described above. Focused correction review found no remaining significant issue:
InitializeLinks is rooted in Arena storage, Node::LinkAt is restored to baseline,
and the 48-row ledger is reconciled. The baseline node-relative post-construction
tail-access idiom remains an explicit strict-provenance portability limitation,
not a new guarantee or regression introduced by this batch.
