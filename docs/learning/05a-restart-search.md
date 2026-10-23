# Study Unit: Searching a Prefix-Compressed Block

[Learning path](README.md) | Parent: [Tables, filters, and caches](05-tables-filters-and-caches.md)

Prerequisites: binary strings and lexicographic order from
[lesson 02](02-bytes-and-formats.md). No engine source is required for the
derivation. Read the source mapping only after attempting the exercises.

## 1. Problem and specification

Store sorted, variable-length binary keys compactly, but still answer a fresh
lower-bound query:

```text
Given target t, return the first key x with x >= t, or report no such key.
```

Lower bound is not exact membership. A missing target may still have a
successor. We study a single valid, immutable block, not database-level
snapshot selection, file I/O, or a previously positioned iterator.

For a nonempty block, define:

| Symbol | Meaning |
|---|---|
| n | Number of entries |
| k | Positive restart interval |
| R | Number of designated restart entries, ceil(n / k) |
| L | Maximum decoded key length in bytes |
| s[j] | Entry position of restart j; here j * k |
| key[i] | Full logical key of entry i |

Keys are strictly increasing under bytewise order in the executable example.
The search proof needs a consistent sorted order and can be extended to
nondecreasing keys. Each entry stores a shared-prefix length and suffix:

```text
key[i] = prefix(previous decoded key, shared[i]) || suffix[i]
```

Every designated restart has `shared = 0`, and the first entry is a restart.
The converse is false: an ordinary entry may also have no common prefix.
The last region can contain fewer than k entries.

Actual blocks locate restarts with **byte offsets**. The teaching model uses
entry positions to make the trace readable; it never binary-searches arbitrary
entry positions. Values and physical varint parsing are outside this model.

## 2. Derive the algorithm from two unsuccessful alternatives

An uncompressed array can binary-search any full key, but repeats common
prefix bytes. Prefix compression saves those bytes by making reconstruction
depend on the preceding key.

Starting from the first entry and decoding until the answer is correct, but
can visit all n entries. Binary-searching arbitrary suffixes is not correct:
the suffix is not the key, and its missing prefix may require earlier entries.

Introduce a small independently decodable index instead. Restart keys are
complete, so binary-search them. Then reconstruct only a nearby run.
The design exchanges extra full keys and offset metadata for bounded replay.

Which restart should we select? Use the **last restart strictly before t**,
falling back to restart zero when none is before it. This is the fresh-seek
choice used by the engine. It also preserves first-equal semantics if we
later allow duplicate keys: jumping to the last restart at most t could skip
earlier equal entries. Under strict uniqueness that alternative can be safe,
but it is a different policy; exercise 8 exposes the distinction.
Strict-before selection may decode the first entry of the next region;
do not assume the replay count is always at most k.

## 3. Algorithm

First handle an empty logical block. For a nonempty valid block:

```text
left = 0
right = R - 1
while left < right:
    middle = left + floor((right - left + 1) / 2)
    if restart_key[middle] < t:
        left = middle
    else:
        right = middle - 1

cursor = restart_offset[left]
previous_key = empty
while cursor is before the entry-region end:
    read the next header and suffix; advance cursor past this entry
    key = prefix(previous_key, shared) || suffix
    if key >= t:
        return this entry
    previous_key = key
return no such entry
```

The middle index rounds upward. If only two candidates remain and the lower
one is kept by `left = middle`, rounding downward could make no progress.
The first decoded entry has shared length zero, so an empty previous key is
sufficient to start reconstruction.

## 4. Correctness, not just a plausible trace

### Restart-search invariant

Let:

```text
a = max({0} union {j : restart_key[j] < t})
```

The fallback zero is part of this definition. It does **not** assert that the
first restart key is smaller than the target.

Maintain:

```text
0 <= left <= a <= right < R
```

**Initialization.** `a` is a valid restart index, so `[0, R-1]` contains it.

**Maintenance.** The upper middle is greater than left while the loop runs.
If its key is smaller than t, sorted restart keys imply `a >= middle`;
keeping `[middle, right]` preserves the invariant. Otherwise no restart at
or after middle is strictly smaller. Since middle is at least one, even the
fallback zero lies before it; keeping `[left, middle-1]` preserves `a`.

**Termination.** Each branch strictly reduces `right-left`. At termination
left equals right, so the invariant gives `left = a`.

Notice why the tempting invariant `restart_key[left] < t` is wrong:
a target before the first key violates it before the first iteration.

### Replay invariant

At the beginning of each replay iteration:

1. every earlier entry has key smaller than t;
2. the previous reconstructed key is correct, or this is the selected
   restart whose shared length is zero.

**Initialization.** If the selected restart is not zero, its key is strictly
smaller than t. Sorted order makes every preceding entry smaller too.
If it is zero, no entry was skipped. Reconstruction starts independently.

**Maintenance.** A valid prefix and suffix reconstruct the next full key.
If it is smaller than t, making it the previous key preserves both claims.

**Successful termination.** The first reconstructed key at least t has no
earlier qualifying entry, so it is exactly the lower bound.

**End termination.** If replay reaches the entry-region end, every entry is
smaller than t. Reporting absence is correct.

These arguments assume valid prefixes, restart positions, and sorted keys.
They do not justify unchecked parsing of arbitrary bytes.

## 5. Worked encoding and trace

Use twelve keys and k = 4. All strings below denote ASCII byte strings.

| i | Key | shared | suffix | Restart? |
|---|---|---|---|---|
| 0 | `ant` | 0 | `ant` | yes |
| 1 | `anteater` | 3 | `eater` | no |
| 2 | `antelope` | 4 | `lope` | no |
| 3 | `bat` | 0 | `bat` | no |
| 4 | `batch` | 0 | `batch` | yes |
| 5 | `bath` | 3 | `h` | no |
| 6 | `cat` | 0 | `cat` | no |
| 7 | `catalog` | 3 | `alog` | no |
| 8 | `cater` | 0 | `cater` | yes |
| 9 | `dog` | 0 | `dog` | no |
| 10 | `dove` | 2 | `ve` | no |
| 11 | `eel` | 0 | `eel` | no |

The restart keys are `ant`, `batch`, and `cater`. Entries such as `bat` and
`cat` are complete keys but are not additional designated restarts.

For target `cat`:

| left | right | middle | Compared key | Next interval |
|---|---|---|---|---|
| 0 | 2 | 1 | `batch < cat` | [1, 2] |
| 1 | 2 | 2 | `cater >= cat` | [1, 1] |

Replay starts at entry 4, not at a suffix in the middle:

```text
entry 4: empty prefix + batch = batch < cat
entry 5: first 3 bytes of batch + h = bath < cat
entry 6: empty prefix + cat = cat >= cat; return entry 6
```

Check different boundary cases before reading the answer columns:

| Target | Lower bound | Restart entry | Restart comparisons | Decoded entries |
|---|---|---|---|---|
| `cat` | `cat` | 4 | 2 | 3 |
| `cater` | `cater` | 4 | 2 | 5 |
| `can` | `cat` | 4 | 2 | 3 |
| `aardvark` | `ant` | 0 | 1 | 1 |
| `zoo` | `none` | 8 | 2 | 4 |

`cater` is the important counterexample to a k-entry replay claim.
Strict-before selection chooses `batch`, then decodes entries 4 through 8.
This crosses into the first entry of the next region.

## 6. Cost model and parameter tradeoff

### Count operations before counting bytes

The binary-search interval has at most half as many candidates, rounded up,
after each comparison. It needs at most `ceil(log2 R)` restart comparisons.
For R = 1 it needs zero.

If another restart follows the selected one, its key is at least t.
Replay therefore stops no later than that next restart, visiting at most
k+1 entries. In the last region, it can visit at most k. A target before the
first key stops after one. A uniform bound is:

```text
decoded entries <= min(n, k+1)
key comparisons <= ceil(log2 R) + min(n, k+1)
entry-level search cost = O(log R + k)
```

This is a bound for a **fresh search**, not a claim about every
state-reusing iterator path or the elapsed time of a database Get.

### Variable-length keys change the unit

Comparing or reconstructing an entry is not necessarily a unit-time
operation. For bytewise comparison and keys at most L bytes, a conservative
CPU-work bound is:

```text
O((log R + k) * (1 + L))
```

The 1 keeps header/position work visible even for empty keys. The decoder
needs one working key of O(L) space, not all replayed keys simultaneously.
An arbitrary custom comparator needs its own cost model. Reading or
decompressing a cold block is another cost, not included in this bound.

### Space is more than the offset array

With four-byte restart offsets and a four-byte count, nonempty blocks use:

```text
restart metadata bytes = 4 * ceil(n / k) + 4
```

Smaller k adds offsets **and** stores more complete keys. This formula counts
only the offset array/count, not those full-key bytes, entry headers,
values, compression, or allocator overhead.

For n = 12, compare a metadata budget of twenty bytes using this deliberately
limited model:

| k | R | Metadata bytes | Binary bound | Replay bound | Total comparison bound |
|---|---|---|---|---|---|
| 1 | 12 | 52 | 4 | 2 | 6 |
| 2 | 6 | 28 | 3 | 3 | 6 |
| 4 | 3 | 16 | 2 | 5 | 7 |
| 8 | 2 | 12 | 1 | 9 | 10 |

Only 4 and 8 fit this metadata budget, and 4 has the smaller comparison
bound. This is not a recommendation to change the engine's default.
The best real interval depends on prefix lengths, lookup distribution,
comparison/copy cost, cache behavior, and the full encoded-size budget.
Big O alone cannot choose it.

## 7. Exercises: derive, falsify, and transfer

Attempt these without the source or executable model. Write assumptions
beside each answer.

1. **Trace.** Reproduce the two search phases for `can` and `zoo`. Explain
   why absence of `can` does not mean lower-bound failure.
2. **Invariant.** Give a target disproving `restart_key[left] < t`.
   State an invariant that remains true for that target.
3. **Termination.** With restart keys `ant`, `batch` and target `zoo`,
   replace the upper middle with a floor middle while retaining
   `left = middle`. Show a complete non-progressing iteration.
4. **Boundary.** Construct a query that reaches k+1 decoded entries.
   Does k = 1 imply only one entry is ever decoded?
5. **Cost.** Under the twenty-byte offset-metadata budget above, choose
   between k = 4 and k = 8. Explain two excluded costs that could change a
   real workload decision.
6. **Expected cost.** Choose a full region other than the first. Suppose
   the target is uniformly one of its k stored keys. Count replay for its
   first key separately and derive the expected number of decoded entries.
   Compare with the first region.
7. **Representation.** Which assumptions fail if a designated restart has
   a nonzero shared prefix, or if the restart byte offset lies outside the
   entry region? Why is an encoded block envelope insufficient?
8. **Variant.** Allow equal keys in a nondecreasing sequence. Does the
   strict-before algorithm still return the first qualifying entry? Would
   selecting the *last* restart at most t remain safe?
9. **Implementation and transfer.** Implement this search in a language of
   your choice. Check it against an independent lower-bound function on
   empty, binary, long-prefix, present, and absent keys. Then adapt the
   ordering to a non-bytewise comparator without changing reconstruction.
10. **Evidence.** A new implementation has the same asymptotic bound but
    fewer outlined calls. Which behavioral, generated-code, and workload
    claims still require separate evidence?

<details>
<summary>Hints and answer checks</summary>

1. `can` returns `cat` after three reconstructions from entry 4. `zoo`
   visits the four-entry final region and has no successor.
2. `aardvark` makes the tempting predicate false. The interval invariant
   must contain the fallback-aware answer a, not assert that predicate.
3. Floor middle is zero, its key is smaller, and left stays zero with right
   still one. The interval width did not decrease.
4. The worked target `cater` decodes five keys for k = 4. With k = 1,
   an exact restart other than the first can require its predecessor and
   itself: two entries.
5. k = 4 gives bound seven rather than ten among the feasible choices.
   Full-key duplication and unequal byte/comparison costs are examples of
   excluded costs; a hardware timing conclusion does not follow.
6. The first key requires k+1 entries from the previous restart. Position
   j = 1 through k-1 requires j+1. Thus the mean is
   `((k+1) + sum(j+1, j=1..k-1)) / k = (k+3)/2`.
   The first region instead averages `(k+1)/2`. A partial final region or a
   different target distribution needs a different calculation.
7. Independent reconstruction or safe cursor formation is lost. The real
   reader must establish bounds and prefix/entry validity before use;
   checking the trailing array's size does not establish all of them.
8. The proof still works with nondecreasing keys. Last-at-most selection
   can skip an earlier equal run: for `a,a,a,b` with k = 1, selecting the
   last `a` misses the first one.
9. Keep the comparator consistent with the sorted order. Prefix sharing
   remains a byte representation operation, not comparator equality.
   The model below intentionally fixes bytewise order; changing that model
   requires updating its sorted-input contract too.
10. Tests/oracles support outcomes and boundary handling; actual caller
    inspection supports generated work; controlled end-to-end trials
    support only their declared workload effect.

</details>

## 8. Execute the model, then open the implementation

The standard-library-only [Python model](examples/restart_search.py) is
executable pseudocode for entry-level reasoning. It needs Python 3.10 or
newer, not an engine build or a database:

```bash
python3 docs/learning/examples/restart_search.py
python3 -m unittest discover -s tests/tools -p restart_search_lesson_test.py
```

On Windows:

```powershell
python docs\learning\examples\restart_search.py
python -m unittest discover -s tests\tools -p restart_search_lesson_test.py
```

It checks builder inputs and constructs immutable prefix entries/restarts.
Search assumes that valid model; it is **not a malformed-SST parser**.
The tests compare every subset of a small binary-key universe to an
independent lower bound, exercise long prefixes, and check the actual
worked encoding/query/cost tables above. Finite exhaustive cases support
the argument; the invariants prove it for the stated model.

| Teaching object | Engine mapping | Additional obligation |
|---|---|---|
| Entry position s[j] | Fixed32 restart byte offset | Check the offset before forming a pointer |
| Prefix/suffix pair | Three varints, suffix, and value | Bound header/payload and validate reconstruction lazily |
| Bytewise keys | Arbitrary block keys or internal keys with their comparator | Internal-key ordering is not raw bytewise ordering of the trailer |
| Fresh lower bound | `FindRestart`, `RestoreCheckpoint`, `ScanToLowerBound` | A valid existing cursor can reuse work; that is outside this fresh-search bound |
| Logical empty model | Empty encoded block | The actual builder writes a zero-offset restart sentinel and count: eight bytes |

Now read [`block.cc`](../../src/table/block.cc),
[`block_builder.cc`](../../src/table/block_builder.cc), and
[`block_test.cc`](../../tests/unit/table/block_test.cc).
Identify the model invariant beside each relevant branch before following
any performance-specific key-borrowing or reverse-replay optimization.
