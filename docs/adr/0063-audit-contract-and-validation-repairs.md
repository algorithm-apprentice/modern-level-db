# ADR-0063: Audit Contract and Validation Repairs

## Status

Implemented as one sequential maintenance slice. The design was recorded
before implementation; the slice changes no public signatures, persistent
formats, comparator semantics, diagnostic report schema, or durability
guarantees.

One bounded GPT-5.6 Sol review of the design and exact implementation found
no actionable issue. The new fixture failed before the policy repair and then
verified nonempty/empty selections for all nine visible test presets. Native
consumer/storage tests, affected ASan/UBSan and model cases, installed-LLVM
fuzz smoke, profiling contracts, diagnostic work accounting, and read-hook
symbol isolation passed before delivery. GCC 13 CI remains the coverage gate.

## Context

A whole-project audit found incomplete public contracts, stale descriptions
of validation boundaries, profiling-only upward dependencies, and validation
gates that can hide missing evidence. The follow-up must verify each finding
instead of interpreting every documentation disagreement as a runtime defect.

| Finding | Verified disposition |
|---|---|
| Comparator and Bloom requirements are absent from public headers | Documentation defect; ADR-0038 already makes compatible equality the caller's responsibility |
| Older ADRs promise eager block/key-order validation | Supersession is incomplete; ADR-0055 and the implementation deliberately use lazy data decoding and writer-established key order |
| Low-level files include `engine/read_diagnostics.h` | Profiling-only layering defect; the ordinary library compiles the hooks out |
| CTest reports success when its selection contains no tests | Reproduced fail-open validation gate, including after a compiler/cache change disabled fuzz targets |
| Three coverage line exclusions have no local reason; two blanket branch exclusions violate ADR-0019 | Maintenance and evidence defects, not proof of an uncovered runtime bug |
| Several implemented ADRs still describe proposed/future delivery | Documentation maintenance defect |
| The local LLVM installation was reported missing | Incorrect diagnosis: the default compiler was Apple Clang, while Homebrew LLVM and libFuzzer were installed |

The comparator/Bloom counterexample does not justify rejecting all custom
comparators or changing filter behavior. Comparator equality may identify
different byte strings when Bloom filtering is disabled. Likewise, restoring
eager block scans would undo the accepted pinned-LevelDB read-path design.

## Prior art and existing contracts

- Pinned LevelDB's `include/leveldb/comparator.h` documents concurrent calls,
  three-way ordering, persistent comparator identity, and the optional
  separator/successor bounds. Adopt those existing contracts without adding
  a comparator capability flag or a key-normalization API.
- ADR-0038 assigns Bloom compatibility to the caller. An arbitrary comparator
  cannot be proven byte-equivalent by checking a few input samples.
- ADR-0003 forbids upward dependencies. The existing read instrumentation
  owns only thread-local collection state and standard-library types, so it
  can be a leaf without changing engine policy.
- CTest's test-preset `execution.noTestsAction = "error"` is the established
  fail-closed mechanism. No wrapper runner is needed.
- ADR-0019 prefers deleting impossible checks and giving temporary objects
  unconditional lifetimes over blanket coverage exclusions.
- CMake documents compiler selection as configuration state; a fresh cache
  and explicit compiler paths are required when switching toolchains.

## Decisions

### 1. Publish existing contracts and mark historical decisions accurately

Document in `Comparator` that comparisons provide deterministic, stable
ordering/equality and that methods may be called concurrently. State the
return-sign contract, persistent-name requirement, and these optional
shortening bounds:

- If the original `start` precedes `limit`, the resulting separator remains
  in `[original start, limit)`.
- A successor compares not less than the original key.
- Leaving either key unchanged is correct.

Document beside `Options::bloom_bits_per_key` and in the README that enabling
Bloom requires comparator equality to imply byte equality. Otherwise leave
Bloom disabled. `Database::Open` does not acquire a new rejection rule.

Amend ADR-0025's current-contract summary: construction bounds the restart
region, data entry validation is lazy, index/metaindex validation walks
physical entries and restart topology, and no reader validates key order.
Mark ADR-0046's eager-validation implementation as historical and superseded
by ADR-0049 and ADR-0055; preserve its original experiment and evidence.
The general rule that unchecked paths need a proved boundary remains valid.

Correct the delivery status of ADR-0019, ADR-0020, ADR-0054, ADR-0056,
ADR-0061, and ADR-0062 using their merged implementation history. Do not
rewrite historical context as though the original design had never existed.

### 2. Make optional read instrumentation an independent leaf

Move the existing pair to `src/instrumentation/read_diagnostics.{h,cc}`.
`instrumentation` is an internal layer-zero leaf depending only on the
standard library, not on `engine`, `api`, platform services, or storage
decoders. Higher layers emit passive counters/events into it.

Update all include paths, the CMake source list, the architecture/DAG, and
current documentation references together. Preserve the namespace, API,
counter/stage names, thread-local lifetimes, report schema, and compile flag.
Do not introduce an interface, callbacks, a runtime switch, or a public header.

The ordinary library still defines `MODERN_LEVELDB_READ_DIAGNOSTICS=0`; only
the separately compiled diagnostic library enables it. This instrumentation
is distinct from layer-seven `diagnostics`, which dumps storage files.

### 3. Fail when a requested test tier does not exist

Add one hidden test-preset base setting `execution.noTestsAction` to `error`
and make every visible test preset inherit it. Add `--no-tests=error` to
the CI CTest commands that do not use a preset.

A fast, dependency-free CMake integration test copies the actual presets
into a generated fixture. For every visible preset it must demonstrate both
one correctly labelled passing test and a nonzero result when a selector
matches nothing. It must exercise CTest rather than merely inspect JSON.
Observe this regression fail before changing the preset policy.

Keep compiler selection explicit, not auto-detected or hard-coded in shared
presets. Add a complete Homebrew LLVM command using `brew --prefix llvm`
and `--fresh`, and explain why installed keg-only tools need not be on PATH.
Do not install or replace an already available toolchain.

### 4. Remove blanket coverage exclusions without weakening validation

Add the existing size-limit reason to each bare line marker.

In `Block::Iterator::Prev`, a checked restart is strictly before the original
valid entry. Each successful parse begins before that entry and therefore
cannot return an invalid end position. Retain checked decoding and explicit
error propagation; assert this invariant in Debug and terminate the loop
using only `next_ < original`. This removes the redundant runtime guard and
its blanket branch exclusion.

Give `ValidateIndex`'s `Block::EntryVisitor` a named, unconditional local
lifetime before passing it to `ValidateEntries`, instead of constructing a
temporary inside the return expression. Remove the blanket closure-cleanup
branch exclusion. Retain only independently justified GCC source-attribution
line markers if the authoritative report requires them.

Do not weaken the coverage policy, guess new GCC branch counts, or exclude
reachable corruption paths to make a report green. GCC 13 in CI remains the
authoritative branch graph; local GCC/LLVM reports are supporting evidence.

## Implementation order and verification

Complete one maintenance branch and one owner-review PR, without starting an
unrelated feature or read-path experiment:

1. Review this design and correct contract/status documentation.
2. Add and observe the failing CTest regression; repair the gate and toolchain
   instructions, relocate instrumentation, and clean up coverage expressions.
3. Review the exact implementation and run the relevant validation.

Required evidence:

- The fixture covers every visible test preset's nonempty and empty selection.
- Existing block forward/reverse/model and corruption tests pass before and
  after the behavior-preserving coverage refactors.
- Unit and CMake-consumer builds preserve ordinary-library behavior.
- Diagnostic mmap/copied-read captures retain the same schema and exact work
  accounting; ordinary throughput executables contain no read-hook symbols.
- No source/build references or Markdown links retain the old instrumentation
  path, and no lower layer includes an engine/API header. Historical prose
  may name the old location to explain the relocation.
- Fuzz smoke runs with the installed full LLVM compiler pair. Run clang-tidy
  against a compilation database from the matching toolchain and distinguish
  existing style suggestions from new defects.
- Relevant sanitizer and GCC 13 changed-code coverage checks remain green.

No throughput admission experiment is required: no algorithm, workload,
cache policy, synchronization, or format is being optimized.

## References

- [ADR-0003 layering](0003-layered-dependency-architecture.md)
- [ADR-0019 coverage](0019-test-coverage-policy.md)
- [ADR-0038 public contracts](0038-public-raii-api.md)
- [ADR-0055 current block validation](0055-leveldb-block-iterator-parity.md)
- [Pinned LevelDB comparator](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/include/leveldb/comparator.h)
- [CMake 3.25 test presets](https://cmake.org/cmake/help/v3.25/manual/cmake-presets.7.html#test-preset)
- [CMake fresh configuration](https://cmake.org/cmake/help/v3.25/manual/cmake.1.html#cmdoption-cmake-fresh)
