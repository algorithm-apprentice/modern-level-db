# 03. Memory and MVCC

[Learning path](README.md) | Next: [WAL and recovery](04-wal-and-recovery.md)

Prerequisite: [internal keys](02-bytes-and-formats.md).
Use [the reasoning toolkit](00-reasoning-toolkit.md) for the probability
and invariant arguments below.

## Lesson map

| Question | Answer |
|---|---|
| What problem is this lesson about? | Keeping recent versioned entries ordered and visible to concurrent readers before they become table files |
| Who consumes this layer? | DatabaseEngine writes and reads, flush, snapshots, and internal iterators |
| What interface matters first? | MemTable `Add`, `Lookup`, `Iterator`, and read pins; SkipList ordered insert/seek |
| What implementation will we open? | Arena allocation, packed entries, SkipList publication, sequence visibility, and tombstones |
| What should you be able to do afterward? | Explain which version a snapshot sees and why readers can traverse while one writer inserts |

## Why an arena fits a memtable

The abstract component accepts unique versioned records, maintains their
internal-key order, and finds a lower bound for a prepared target. Reads
must return the newest visible value/deletion for their sequence, while
one serialized writer can add records. No node is removed during the
component's live lifetime. Let n count entries and L bound key bytes.

A sorted array makes search simple but insertion can shift many entries;
a balanced tree changes shape and needs its own concurrent-reader proof.
The arena/skip-list combination instead exploits append-only lifetimes and
immutable published node contents. The following reasoning is conditional
on those constraints, not a general concurrent-map recipe.

Memtable entries are created together, remain at stable addresses, and are
reclaimed together when the memtable is no longer needed.
Individually allocating and freeing every entry adds allocator bookkeeping
without matching that lifetime.

An **arena** reserves blocks and advances an allocation pointer:

```text
arena block:
[entry A][entry B][alignment padding][skip-list node][unused space]
                                             allocation pointer ^
```

Arena returns the address of raw storage. The caller already knows the
requested size: MemTable wraps the pointer and encoded size in a writable byte
view, while SkipList uses the aligned pointer for placement construction.
This keeps logical range bounds with the consumer rather than repeating them
in the allocator's return value.

The current implementation uses 4 KiB normal blocks and dedicated
allocations for requests larger than 1 KiB when it needs a fallback.
It does not free individual entries. Destroying the arena releases its
owned blocks.

`memory_usage()` measures reserved block bytes plus the accounting for
block-owner pointers, not just key/value payload bytes.
That distinction matters because the engine uses it to decide when to
rotate a memtable.

Alignment makes an address suitable for a type; it does not construct that
type. Skip-list nodes and their trailing atomic links are explicitly
constructed in aligned arena storage.

### Analyze the lifetime match, not just allocation speed

The model has append-only allocations whose last borrower leaves before the
whole memtable is destroyed. Under that condition, bulk reclamation removes
the need to maintain an individual free operation for each entry.

In a toy sixteen-byte arena block, three five-byte requests use fifteen
payload bytes while reserving sixteen. A fourth needs another block:
payload becomes twenty and reservation thirty-two. Alignment padding and
allocator/owner metadata would add further differences. Reserved bytes and
live user bytes are not interchangeable units.

For request i with alignment a[i], padding is less than a[i]. Total storage
must cover payload, padding, and block slack. Bump advancement is constant
metadata work; acquiring backing memory and touching its pages have their
own costs. Do not call every allocation constant-time without that model.

The lifetime assumption is essential. An independently freed object or a
borrower surviving arena destruction requires a different ownership design,
not a faster bump pointer.

## The skip list

A skip list is an ordered linked structure with additional shortcut levels:

```text
level 2: head --------------------> M --------------------> Z
level 1: head -------> D ---------> M ---------> T -------> Z
level 0: head -> A -> D -> H -> K -> M -> P -> T -> W -> Z
```

Search moves forward while it can, then drops to a lower level.
Random node heights permit expected logarithmic search in the growing-height
model below, without tree rotations. This implementation has branching
factor four and maximum height twelve; a fixed cap limits that asymptotic
claim.

### Derive search and its invariant

Treat the head as smaller than every key:

```text
node = head
for level from the highest active level down to zero:
    while node.next[level] exists and node.next[level].key < target:
        node = node.next[level]
return node.next[0]
```

The returned node is either the first key at least target or absent.
Maintain that node is the head or a key smaller than target, and every
level-zero entry at or before it is excluded from the answer. Advancing
to a smaller next key preserves that claim by sorted order; dropping a
level excludes nothing new. At level zero, the next entry is the first
not excluded, and the loop has established that it is not smaller.

Forward moves and the finite number of level drops establish termination.
This proof assumes sorted links and stable, fully initialized reachable
nodes. It does not establish concurrent publication; that is a separate
argument below and in lesson 08.

### Derive expected space and search cost

In an idealized model, promote each node independently with probability
`0 < p < 1`. A node reaches zero-based level j with probability p^j, so
linearity of expectation gives n*p^j nodes at that level.
For n = 1024 and p = 1/4:

<!-- skip-height-model -->

| Level j | Probability of reaching j | Expected nodes |
|---|---|---|
| 0 | 1 | 1024 |
| 1 | 1/4 | 256 |
| 2 | 1/16 | 64 |
| 3 | 1/64 | 16 |
| 4 | 1/256 | 4 |
| 5 | 1/1024 | 1 |

Each node needs one link per reached level. Summing the geometric series
gives expected links per node `1/(1-p)`, or 4/3 for p = 1/4 before a cap.
With cap M it is `(1-p^M)/(1-p)`, excluding the head and allocator overhead.

For search, reason backward through a search path: reaching a predecessor
that also appears one level higher needs a geometrically bounded number
of trials, with expected O(1/p) horizontal work per level. When useful
height grows on the order of `log_(1/p) n`, the resulting expected path
cost is `O((1/p) * log_(1/p) n)` under the random-height assumptions.
This is expectation over the shape, not a bound for every possible shape.

A fixed height cap changes the large-n model. The highest level alone has
expected n*p^(M-1) nodes; a far-right search can traverse them. A useful
capped bound is `O(n*p^(M-1) + M/p)`. For constant M and p, it is not
asymptotically logarithmic as n grows without limit. The implementation's
finite cap is an engineering choice, not a stronger mathematical theorem.
Variable-length comparisons add their byte cost to these path counts.

The SkipList is an ordered set whose Entry type is a pointer to one packed
MemTable record. That referenced record contains both the internal key and
value; the comparator extracts only the internal key for ordering.

Read searches pass the already prepared internal-key view to the comparator;
only stored records need length-prefix decoding at each comparison. Dropping
a level can encounter the same upper-bound node again. The search remembers
that immutable node's comparison result instead of comparing it twice.
Insertion still collects every level's predecessor before publishing links.

Targets convertible to Entry use the original stored-entry overload and
convert once before searching, including an empty list. Only distinct
non-convertible prepared representations select the heterogeneous overload;
implicit conversions do not move into the comparison loop. Selection preserves
the argument's qualification and value category.

The concurrency contract is deliberately narrow:

- One externally serialized writer inserts nodes.
- Concurrent readers follow published links.
- Nodes are never removed while the memtable is live.

This avoids concurrent node reclamation and greatly simplifies reader
lifetime safety. It is not a general multi-writer lock-free container.

`Next` follows a level-zero link. `Prev` searches for a predecessor; it is
not a backward link. Forward and reverse traversal need not have equal cost.

## Packed entries

A memtable entry contains:

```text
varint32(internal-key length)
user key
eight-byte sequence/kind trailer
varint32(value length)
value
```

A deletion uses kind Deletion and an empty value.
Lookup needs only the internal key. Its search view does not need a packed
record's length prefix or value field, and an iterator does not copy the
target into a temporary encoded record.

The representation is compact, stable, and directly comparable.
Returned internal views borrow arena storage; the public `Get` copies the
found value into caller-owned storage.

## MVCC: choose a version, not a mutable slot

**Multi-version concurrency control**, or MVCC, keeps history so a reader
can choose a visibility boundary.
Modern LevelDB uses monotonically assigned sequence numbers rather than
wall-clock timestamps:

```text
color@42 Deletion
color@41 Value "blue"
color@40 Value "red"
```

For snapshot sequence `S`, seek the first entry for the key with
`sequence <= S`.

### Why an internal-key lower bound implements visibility

Order one user key's records by decreasing sequence, then decreasing kind.
Construct the target `(user key, S, Value)`: Value=1 is the greatest supported
kind, above Deletion=0. The target sorts after all newer records and before
all older ones. At sequence S it equals a Value record and precedes a
Deletion record, so neither eligible kind is skipped.

A lower bound therefore reaches the newest eligible record. The resulting
user key still needs an equality check: the next larger user key is not a
value for this one.

The kind is a necessary premise, not an incidental encoding detail.
For a separate history containing `color@40 Value` and `color@39 Value`,
compare targets at S=40. Trailers pack `(sequence << 8) | kind` and compare
numerically in descending order:

<!-- mvcc-seek-kind-example -->

| Target kind | Packed target trailer | First found sequence |
|---|---|---|
| Value | 10241 | 40 |
| Deletion | 10240 | 39 |

The wrong Deletion target sorts after the visible Value at 40 and misses it.
Sequence alone does not define the required lower-bound position.

If the first eligible record is a deletion, absence is decided immediately.
Continuing to an older value would violate the logical history definition,
not merely choose a slower search.

| Visible sequence | First visible entry | Result |
|---|---|---|
| 39 | None of these entries | Missing, unless an older source has history |
| 40 | Value at 40 | `"red"` |
| 41 | Value at 41 | `"blue"` |
| 42 | Deletion at 42 | Absent; stop searching |

The snapshot is a sequence registration, not a copy of the data.
Compaction must retain the versions needed by registered snapshots.

## Atomic batches and visibility

Suppose a two-operation batch gets sequences 50 and 51.
While its entries are being inserted, the published last sequence remains
49. After both insertions succeed, the engine publishes 51.

New latest reads therefore choose either the pre-batch or post-batch
boundary, not the artificial boundary between its operations.
An explicit snapshot also captures a published boundary.

The publication argument has three premises: one serialized commit controls
the interval, inserted nodes are safely readable, and final sequence
publication occurs only after all entries are inserted. A reader capturing
49 ignores even already linked records 50 and 51. A reader capturing 51 can
encounter both completed insertions. Publishing 51 after inserting only
record 50 would admit a counterexample: the reader could observe the first
update but not the second.

This does not make separate reads a transaction:

```text
Get("a") -> concurrent batch commits -> Get("b")
```

Those calls can choose different latest sequences.
Use one explicit snapshot or a one-sequence MultiGet when several reads must
share a view.
Atomic write batches do not provide conflict detection or read-modify-write
transaction semantics.

## Logical retention versus physical retention

Two different lifetimes protect readers:

| Protection | What it preserves |
|---|---|
| Snapshot registration | History required in future compacted versions |
| Memtable/version/table/cache pin | Storage already being read |

A normal iterator captures a sequence and retains its source memtables and
Version. Its old files stay available even if background work installs new
ones. An iterator created from an explicit Snapshot additionally retains
that registration.

Long-lived snapshots can retain old entries in future compaction outputs.
Long-lived iterators can retain old source files.
Both can increase resource use without copying the entire database.

## Exercises: search, probability, and visibility

1. Why can deleting `"color"` not simply remove its newest memtable entry?
2. Why can a reader safely hold a pointer while another thread inserts?
3. Does a 4 MiB write buffer imply a strict 4 MiB database memory limit?
4. What must retain the arena if a lookup or iterator uses a packed-entry
   pointer after the database mutex is released?
5. **Derive:** reproduce the height table and expected link count. Does
   summing expectations require independence? Which other step does?
6. **Limit:** can the twelve-level implementation promise expected O(log n)
   for arbitrarily large n? State the missing growth assumption.
7. **Proof:** for records at sequences 42, 41, and 40, explain every entry
   excluded by a lower bound at S = 40. What if it reaches another user key?
8. **Counterexample:** move batch sequence publication before its final
   insertion. Give a read interleaving violating atomic visibility.
9. **Transfer:** suppose nodes can be removed while readers traverse.
   Which premise of the concurrency/lifetime proof is lost? Does release
   publication alone solve reclamation?
10. **Boundary:** reconstruct the seek-kind table from numeric trailers.
    Why would adding a new kind above Value require reconsidering the seek
    target rather than only extending the enum?

<details>
<summary>Answers</summary>

1. Older values may exist in other sources, and snapshots may need them.
   A tombstone hides them for newer readers.
2. The writer publishes initialized nodes, nodes do not move or disappear,
   and a retained memtable keeps the arena alive. The publication details
   are covered in lesson 08.
3. No. There can be mutable and immutable memtables, retained readers,
   caches, metadata, and allocation slack. Rotation also checks usage at
   write boundaries rather than enforcing an allocation cap.
4. An owner must keep the MemTable and Arena alive. The engine's read-pin
   protocol retains a rotated table while its count is nonzero; a standalone
   counter pin does not itself own the table.
5. Reaching level j requires j promotions; its expected population is
   1024/4^j. Adding indicator expectations does not need independence;
   multiplying promotion probabilities does. Expected link count sums the
   height tail, not logarithms of node counts.
6. No. Growing-height analysis needs enough levels. With a fixed cap,
   highest-level traversal eventually dominates.
7. Newer records sort before the target. The first same-key record at most
   S decides the value/deletion because the target uses the greatest supported
   kind; another user key means no eligible record was found in this source.
8. A reader captures the new final sequence between the two insertions and
   misses the not-yet-inserted member of the batch.
9. Stable lifetime is lost. Publication orders initialization but does not
   prevent a later free while an existing reader still uses the node.
10. At 40, the Value trailer is 10241 and Deletion is 10240. Descending order
    puts Value first. A newly supported greater kind would also sort before
    the old target at equal sequence and could be skipped.

</details>

## Source tour

| Entry point | Interface or symbol | Question to answer | Representative evidence |
|---|---|---|---|
| [`arena.cc`](../../src/memory/arena.cc) | `Arena::Allocate`, aligned allocation | What lifetime and accounting does one allocation join? | [`arena_test.cc`](../../tests/unit/memory/arena_test.cc) |
| [`skiplist.h`](../../src/memory/skiplist.h) | `Insert`, search, iterator | What does the single writer establish before release publication? | [`skiplist_test.cc`](../../tests/unit/memory/skiplist_test.cc) |
| [`memtable.cc`](../../src/memory/memtable.cc) | `Add`, `Lookup`, `Iterator`, read pin | How does one packed entry support snapshots and deletions? | [`memtable_test.cc`](../../tests/unit/memory/memtable_test.cc) |
| [`internal_key.h`](../../src/format/internal_key.h) | `SeekValueKind`, `LookupKey::Create` | Why must the target include the greatest supported kind? | [`internal_key_test.cc`](../../tests/unit/format/internal_key_test.cc) |
| [`database_write.cc`](../../src/engine/database_write.cc) | `CommitWrite` | When does a completed MemTable mutation become visible? | [`database_write_test.cc`](../../tests/unit/engine/database_write_test.cc) |
| [`snapshot_registry.h`](../../src/engine/snapshot_registry.h) | opaque linked registration, release, oldest sequence | How do duplicate snapshots keep distinct nodes and the oldest visible sequence? | [`snapshot_registry_test.cc`](../../tests/unit/engine/snapshot_registry_test.cc) |
| [`database_read.cc`](../../src/engine/database_read.cc) | `GetSnapshot`, `ReleaseSnapshot` | Which sequence keeps old state visible to a reader? | [`public_api_native_test.cc`](../../tests/unit/api/public_api_native_test.cc) |

For design history, read [ADR-0015](../adr/0015-concurrent-skiplist.md),
[ADR-0017](../adr/0017-arena-backed-memtable.md), and the memory invariants
in [ADR-0060](../adr/0060-leveldb-write-path-parity.md).
