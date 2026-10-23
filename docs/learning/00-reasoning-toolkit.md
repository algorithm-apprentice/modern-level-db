# 00. A Reasoning Toolkit for Storage Algorithms

[Learning path](README.md) | Next: [The engine map](01-the-engine-map.md)

This course assumes basic programming, not prior database implementation
experience. Use this chapter whenever a later argument uses an unfamiliar
tool. You do not need to read engine source to complete it.

## 1. Start with an observable specification

An implementation name is not a specification. "Use a skip list" does not
say which value an old snapshot must see. "Use a log" does not say which
acknowledged writes survive a power loss.

A useful specification identifies:

```text
input domain -> observable result -> permitted failures
             -> preconditions -> state that must remain unchanged
```

For lower bound on a sorted array a of length n:

```text
return the smallest i with a[i] >= target
return n if no such index exists
```

This differs from exact membership. In `[2, 2, 5]`, target 2 returns zero,
target 3 returns two, and target 6 returns three.

A storage contract additionally needs a time/failure boundary: "the last
published sequence at the beginning of this read" or "writes whose sync
success was acknowledged before the modeled crash." Those are different
specifications, not interchangeable meanings of "latest" or "safe."

## 2. A model states what the argument assumes

A model deliberately leaves things out. Counting key comparisons can
explain a search algorithm without describing page faults. It cannot then
predict disk latency.

Write assumptions where they enter the argument:

| Model assumption | What breaks without it |
|---|---|
| Keys are ordered by the same comparator used for search | Binary search can discard the answer |
| Prefix lengths and entry extents are valid | Reconstruction can read outside the preceding key or block |
| A borrowed object stays alive and stable | A logically correct search can access invalid storage |
| Successful synchronization satisfies the stated backend contract | A durability proof has no persistence barrier to rely on |
| Promotions follow the probability model | An expected skip-list cost need not describe the chosen heights |

An assumption is not automatically a runtime check at every operation.
Identify which checked boundary or construction invariant establishes it
and what prevents later invalidation.

## 3. Prove a loop with three obligations and progress

An **invariant** is a statement true before and after each loop iteration.
It is useful because it connects local steps to the final specification.

Consider this half-open lower-bound search:

```text
left = 0; right = n
while left < right:
    middle = left + floor((right - left) / 2)
    if a[middle] < target:
        left = middle + 1
    else:
        right = middle
return left
```

Its invariant is:

```text
0 <= left <= right <= n
all entries before left are smaller than target
all entries at or after right are at least target
```

**Initialization:** the excluded ranges are empty.
**Maintenance:** sorted order makes the chosen half safe to exclude.
**Postcondition:** when left equals right, no earlier index qualifies and
every remaining index qualifies; left is the requested boundary.

That establishes partial correctness, not termination. For termination,
use the nonnegative integer `right-left`: every iteration strictly decreases
it. Changing an endpoint update without revisiting progress can create an
infinite loop.

The [restart-search unit](05a-restart-search.md) uses a different, inclusive
interval and upper middle. Do not transplant a midpoint or invariant between
the two algorithms without checking their endpoint meanings.

## 4. State the unit before using Big O

Let n describe input size and C describe a counted operation. A bound such
as `O(log n)` is incomplete as a performance explanation until C is clear.

| Counted unit | What it can explain | What it does not include automatically |
|---|---|---|
| Key comparisons | Search path length | Variable-length byte comparison |
| Bytes reconstructed | Prefix-decoding work | Allocation, cache misses, or I/O |
| Blocks read | A chosen external-memory access model | Device latency or compression ratio |
| Bytes written | A declared write-amplification interval | Background work outside that interval |
| Calls remaining in optimized code | One generated-code boundary | Whole-workload speed |

`O(f(n))` is an asymptotic upper bound, not a statement that all inputs cost
the same or that constants are irrelevant. Distinguish:

- **Worst-case cost:** a bound for every input in the model.
- **Expected cost:** an average over an explicitly stated random mechanism
  or input distribution.
- **Amortized cost:** total cost over an operation sequence divided among
  its operations, without requiring random inputs.

For an abstract array that doubles when full, growing through capacity
1, 2, 4, and 8 copies 15 items. Nine appends can therefore include a large
individual copy, while the total copied items remain less than twice nine.
This example assumes unit-cost item copying and that exact growth rule;
it is not a guarantee about every `std::vector` implementation.

If a key has L bytes, one comparison may require O(L) byte work.
For a custom comparator, state its own cost. A constant-cap data structure
also needs care: a fixed maximum height or cache size cannot automatically
support an unbounded asymptotic claim.

## 5. Small probability tools, with their assumptions

For a positive random height H:

```text
E[H] = P(H >= 1) + P(H >= 2) + P(H >= 3) + ...
```

One way to see this is to write H as a sum of indicator variables: one for
each level it reaches. Linearity of expectation lets us add their means;
it does not require those indicators to be independent.

If each promotion independently succeeds with probability `0 <= p < 1`,
and the cap M is a positive integer, then:

```text
P(H >= h) = p^(h-1)
unbounded E[H] = 1 / (1-p)
with height capped at M: E[H] = (1-p^M) / (1-p)
```

The finite expression follows by multiplying the geometric sum by p and
subtracting. For p = 1/2 and M = 3, it is `1 + 1/2 + 1/4 = 7/4`.
The degenerate p = 1 case needs separate treatment: a capped height is always
M, while the uncapped model has no finite expected height.

Independence matters when multiplying probabilities, even though it was not
needed to add expectations. Bloom-filter approximations later use a random
hashing model; a fixed noncryptographic hash is not a theorem about every
adversarial key set.

## 6. State machines and failure cuts are correctness tools

Not every systems argument is a loop proof. A native read can be:

```text
not issued -> pending -> terminal success or terminal failure
```

The pending state can still borrow a buffer after an API reports a wait
error. A valid cleanup transition requires a terminal event, not merely an
error code. The safety statement is "no borrower remains when storage is
released."

For durability, list the dependencies that make a published object
recoverable: contents, durable name, and durable reference to that name.
Then cut the execution at each boundary and enumerate the legal recovered
states. Do not assume durable operations form a prefix of program calls
unless the failure model and barriers establish that ordering.

An in-memory publication point and a durability point need not coincide.
A value can be durable before its caller receives success, and an
asynchronous value can be visible without a power-loss guarantee.

## 7. Proof, examples, and tests have different jobs

A worked example helps a reader execute an idea. A counterexample disproves
a universal claim. An invariant argument covers every input satisfying its
assumptions. A finite test run searches for defects within its exercised
domain; it does not establish the universal claim by itself.

Good evidence is complementary:

```text
specification and assumptions
    -> derivation and correctness argument
    -> independent examples and counterexamples
    -> implementation checks and measured limits
```

Reading the source is the final mapping, not a substitute for understanding
the model. A test that passes because an encoder and decoder share the same
mistake is weaker than comparison to independent golden bytes.

## Exercises

1. **Trace:** run the half-open search for targets 2, 3, and 6 in `[2,2,5]`.
   State the excluded ranges after each iteration.
2. **Proof:** show that `right-left` decreases in both branches. Why is
   `left = middle` not interchangeable with `left = middle+1` here?
3. **Cost:** two algorithms make the same number of comparisons, but one
   compares longer keys. What information is missing from a timing claim?
4. **Derivation:** derive the finite geometric sum instead of memorizing
   it. Explain exactly where independent promotions enter the height model.
5. **Amortization:** for nine appends in the doubling-array model, list the
   copied capacities. Can the ninth append still be individually expensive?
6. **Failure model:** an operation is pending when waiting reports an error.
   Which fact is missing before its buffer can be freed?
7. **Transfer:** choose a data structure you know. Write its specification,
   one invariant, a progress measure, a cost unit, and one assumption whose
   removal creates a counterexample.

<details>
<summary>Hints and answer checks</summary>

1. The answers are 0, 2, and 3; equal values require the first qualifying
   index, not an arbitrary match.
2. With two candidates the lower middle can equal left. The increment is
   essential to exclude that tested smaller value and make progress.
3. Length distribution, comparator work, reconstruction/allocation, and
   memory/I/O behavior are not supplied by a comparison count.
4. For S = `1+p+...+p^(M-1)`, `(1-p)S=1-p^M`. Independence gives the product
   for reaching successive heights; adding indicator expectations does not
   need independence.
5. The copied capacities are 1, 2, 4, and 8. Their sum is 15; amortized
   linear total copying does not make every operation constant-time.
6. The kernel must be unable to access the operation record, event, and
   buffer. Establish terminal completion, including drain if required.
7. A complete answer makes the observable result and model limits explicit;
   an implementation name alone is insufficient.

</details>

Continue with [the engine map](01-the-engine-map.md). Return here when a
later chapter asks for a proof, expected cost, or failure-model argument.
