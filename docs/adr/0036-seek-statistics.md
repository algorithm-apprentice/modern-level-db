# ADR-0036: Seek Statistics

- Status: Accepted
- Date: 2026-09-24

## Context

A file that point reads keep searching without finding their keys costs a
seek on every such read. LevelDB therefore gives each file a seek budget,
charges it for such reads and for sampled iterator reads, and compacts a file
whose budget runs out when no level is full. [ADR-0030](0030-point-reads.md)
and [ADR-0031](0031-iterators.md) left these statistics to the compaction
work, and [ADR-0034](0034-compaction-picking.md) assigned them to this node,
whose seek compaction it picks from a file the caller provides.

This implements only the `implement-seek-statistics` DAG node: the seek
charge of a point read, read sampling in database iterators, and the budgets
and file to compact that the charges produce. Taking the database mutex
around the charges, scheduling the compaction, and seeding each iterator
belong to the engine node.

## Current Modern LevelDB callers

| Future caller | Required behavior |
|---|---|
| Engine point reads | Charge the first file that a read searched, if it searched another, to the version the read used |
| Engine iterators | Sample about one entry per MiB read and charge the first of the current version's files that hold a sampled key, if at least two do |
| Engine background work | Get the current version's file to compact for `PickCompaction` and learn when a charge produced one |

No current caller requires statistics per level, exposing them, or seek
compactions triggered any other way.

## Prior art

### Google LevelDB

Adopt `Version::Get`'s `GetStats`, `Version::UpdateStats`,
`Version::RecordReadSample`, `VersionSet::Builder::Apply`'s `allowed_seeks`,
and `DBIter::ParseKey`'s sampling:

- A file's budget is its size divided by 16 KiB, at least 100. A file that
  enters a version through an edit, including one that a trivial move adds
  to the next level, starts a new budget; a file that versions share keeps
  one budget.
- A point read that reaches the version and searches more than one file
  charges the first file it searched, even if the read fails.
- A database iterator counts the bytes of the keys and values of the entries
  it examines while it looks for the next or previous user key. When the
  count passes a random period, uniform below 2 MiB from LevelDB's
  Park-Miller generator seeded per iterator, it samples the entry's key,
  once for each period passed.
- A sampled key charges the first file of the current version that a point
  read at that key would search, if at least two such files exist: level-0
  files that hold the user key, newest first, and then each deeper level's
  only candidate.
- A charge decrements the file's budget. Once it is at most zero, the file
  becomes the file to compact of the version charged, unless that version has
  one. Only the current version's file to compact starts a compaction, and a
  new version starts without one.

Change:

- **Budgets outside the metadata.** LevelDB decrements a counter in the file
  metadata that versions share. Here file metadata is immutable
  ([ADR-0027](0027-version-set.md)), so `SeekStatistics` keeps a budget per
  shared metadata object, which versions share while they share the file.
  An edit's new file is a new object, so it starts a new budget as in
  LevelDB.
- **One file to compact, tied to its version.** LevelDB keeps one in every
  version, and only the current version's matters. Here `SeekStatistics`
  keeps one for the version that was current when a charge produced it, and
  ignores it once another version is current, as ADR-0034 requires.
- **Failed reads charge nothing.** A read that fails returns only its error;
  the first file it searched is not charged.
- **Samples are internal keys.** LevelDB samples a key before it parses it.
  Here the iterator samples only entries whose keys it parsed, since a key
  that is not an internal key fails the iterator.
- **Injected periods.** The iterator takes a function that returns each
  period and a function that receives each sample, so tests use small fixed
  periods; `ReadSamplingPeriods` returns LevelDB's.

### RocksDB

RocksDB removed seek compaction in version 3.2.0, because it helps spinning
disks more than flash and complicates important code paths. It is kept here
because ADR-0030 and ADR-0031 committed the compaction work to it and the
format oracle's databases compact that way.

## Decision

Change `src/engine/lookup.{h,cc}` and `src/engine/db_iterator.{h,cc}`, and add
`src/engine/seek_statistics.{h,cc}`:

```cpp
struct SeekCharge {
  std::uint32_t level;
  Version::File file;
};

struct PointRead {
  std::optional<std::vector<std::byte>> value;
  std::optional<SeekCharge> seek;
};

Result<PointRead> LookupValue(/* as before */);

std::optional<SeekCharge> SampleCharge(const Version& version,
                                       const InternalKeyComparator& comparator,
                                       ByteView internal_key);

struct ReadSampling {
  std::function<std::uint64_t()> next_period;
  std::function<void(ByteView internal_key)> sample;
};

DbIterator(std::unique_ptr<InternalIterator> internal, const Comparator& user_comparator,
           SequenceNumber sequence, ReadSampling sampling = {}) noexcept;

inline constexpr std::uint64_t ReadBytesPeriod = 1 << 20;

std::function<std::uint64_t()> ReadSamplingPeriods(std::uint32_t seed);

class SeekStatistics final {
 public:
  bool Charge(const std::shared_ptr<const Version>& version,
              const std::shared_ptr<const Version>& current, const SeekCharge& charge);
  std::optional<SeekCompaction> FileToCompact(
      const std::shared_ptr<const Version>& current) const;
  void Retain(const Version& current);
};
```

- `LookupValue` returns the value as before, and, if the read reached the
  version and searched more than one file, the first file it searched and
  its level. Errors are returned as before, without a charge.
- `SampleCharge` returns the first file, with its level, that a point read
  at the valid internal key would search in the version, if it would search
  at least two.
- A `DbIterator` with sampling counts the key and value bytes of each entry
  whose key it parses in its loops that find the next or previous user key,
  calls `next_period` for the first period when it counts its first entry,
  and calls `sample` with the entry's internal key for each period that the
  count passes, as LevelDB's `DBIter::ParseKey` does. The walk that `Prev`
  makes back over the current user key's entries when it changes direction
  validates keys without counting them, as LevelDB's walk does not parse
  them. Without sampling it behaves as before. The constructor stays
  `noexcept`, since it only moves its arguments, and the deleted overload
  that rejects a temporary comparator takes the same optional sampling.
- `ReadSamplingPeriods` returns periods uniform below twice
  `ReadBytesPeriod`, drawn as LevelDB's `Random(seed).Uniform` draws them.
- `SeekStatistics::Charge` decrements the file's budget, starting from its
  size divided by 16 KiB and at least 100, as a signed count that keeps
  falling below zero as LevelDB's does. If the budget is then at most zero,
  `version` is `current`, and no file to compact is recorded for `current`,
  it records the file for `current` and returns true; otherwise it returns
  false. `version` must hold the file at the charge's level.
- Budgets are keyed by the `Version::File` itself, so a budget owns its
  metadata object and no other object can take its place; neither addresses
  nor file numbers identify a budget.
- `FileToCompact` returns the recorded file if it was recorded for
  `current`.
- `Retain` forgets the budgets of files that `current` does not hold and a
  file to compact recorded for another version.
- `SeekStatistics` is not thread-safe; the engine calls it with the database
  mutex held.

The engine charges a point read's `seek` to the version the read captured,
with the current version as `current`; charges each iterator sample through
`SampleCharge` on the current version; calls `Retain` after it installs each
version; considers a compaction when `Charge` returns true; and passes
`FileToCompact` to `PickCompaction`. Each iterator gets
`ReadSamplingPeriods` with a seed from a counter, as LevelDB's `seed_`.

## Explicitly deferred behavior

- The database mutex, scheduling, and iterator seeds, which belong to the
  engine node.

## Validation plan

Unit tests cover:

- `LookupValue`'s charge: none when a memtable decides, none when one file
  decides, the first file when a read searches two or more, across level 0
  and deeper levels, including a read that searches every file and finds
  nothing; and none after an error.
- `SampleCharge` for keys that no file, one file, and several files hold,
  including level-0 files newest first and a deeper level's candidate, and
  samples on both sides of, and at, the boundary between two deeper-level
  files that split one user key's entries.
- `DbIterator` sampling with fixed periods: the bytes counted in both
  directions and while skipping hidden entries, several samples for one large
  entry, the first period drawn at the first entry, no counting in the walk
  that `Prev` makes when it changes direction, no sampling without it, and
  the rejected temporary comparator with sampling.
- `ReadSamplingPeriods` against LevelDB's first periods for fixed seeds.
- `SeekStatistics`: the budget from the file size and its minimum, a file
  to compact once it runs out, only for the current version and only once,
  a budget shared by versions that share the file, a new budget for a new
  metadata object, `Retain`, and an exhausted file charged again while
  another file is recorded, which a new version that shares it records at
  its next charge.
- Every line and branch of the new code, as required by
  [ADR-0019](0019-test-coverage-policy.md).

No differential helper is needed: the charges follow LevelDB's rules
directly and are unit-tested, `LookupValue`'s results were compared with
LevelDB's by ADR-0030's helper, and seek compactions are picked by
ADR-0034's tested `PickCompaction`.

## Consequences

- Seek compactions work as in LevelDB once the engine wires the charges.
- Point reads report a charge that the engine must apply.

## References

- [Google LevelDB `Version::Get`, `UpdateStats`, and `RecordReadSample`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/version_set.cc)
- [Google LevelDB `DBIter::ParseKey`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/db_iter.cc)
- [RocksDB 3.2.0 release notes](https://github.com/facebook/rocksdb/blob/main/HISTORY.md)
