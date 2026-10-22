# ADR-0086: Persisted Full-Table Filters

- Status: Accepted
- Date: 2026-10-22

## Context

Current offset-range filters need an index result before they can select a
filter. The retained experiment excluded 887,528 of 1,895,987 candidate SST
probes before index seek. The filter payload was 1,250,057 bytes over one
million stored keys. The experiment's full-SST scan at Open must not become
production behavior.

A cache-local representation also allows the independently measured SIMD
mechanism to be introduced without changing existing LevelDB Bloom bytes.
The format is an optional SST metadata extension: old readers can ignore it.

This design follows [ADR-0085](0085-reviewed-read-optimization-adoption.md).

## Public selection and compatibility

Add `BloomFilterLayout` to the public options with `Automatic`, `BlockBased`
and `FullTable`. Default to `Automatic`; default `bloom_bits_per_key` to ten.
An explicitly empty optional disables filters regardless of layout.

Automatic selection enables a full-table filter only when the comparator
is the built-in bytewise comparator, selected by a null comparator owner or
its exact known object identity. Do not infer byte equality from a comparator
name, dynamic type or ordering sample. Automatic selection disables filters
for other comparators. Explicit `BlockBased` or `FullTable` selection requires
the existing caller promise that comparator equality implies byte equality.

Validate the layout enumeration at Database Open, even when filtering is
disabled. Map public selection to a lower-layer internal layout; do not add a
dependency from the table implementation to public database options.

The layout controls newly written tables. When filtering is enabled, readers
recognize both legacy and new metadata, so reopening with a different write
selection remains correct. Filter-disabled opens continue to ignore filter
metadata rather than eagerly diagnosing previously ignored filter bytes.

Keep direct internal TableBuilderOptions defaults unchanged: no policy means
no filter, and its explicitly supplied policy defaults to the legacy layout.
The API boundary chooses the new effective default.

No existing SST needs rewriting on Open. Flush, recovery output and compaction
use the captured table-building selection. Old SSTs remain readable.
Native LevelDB and older Modern readers ignore the new metadata name and can
read the same data/index/footer using the existing checksum and compression
contracts. They simply do not obtain the new filter's exclusion benefit.

## Version-one representation

Use the unique metaindex key `filter.modern_leveldb.FullBloom1`.
Do not reuse `filter.leveldb.BuiltinBloomFilter2`.

The block payload is a 16-byte header followed by complete 64-byte regions:

| Bytes | Meaning |
|---|---|
| 0..3 | Little-endian fixed32 magic `0x31464c4d` (`MLF1`) |
| 4 | Probe count, 1 through 30 |
| 5..7 | Reserved zero bytes |
| 8..11 | Little-endian fixed32 region count |
| 12..15 | Reserved zero bytes |
| 16..end | Exactly `region_count * 64` bytes of filter bits |

The region count is at least one. An empty table has one all-zero region.
Require the complete payload to fit the existing uint32 filter-size domain.
Use wide arithmetic at construction/admission boundaries before narrowing.

Version one freezes these computations:

- Bucket hash: existing output-stable Hash32 with seed `0xbc9f1d34`.
- Probe hash: Hash32 with seed `0x9e3779b9`.
- Region: the high 32 bits of the uint64 product of bucket hash and region
  count, avoiding division while selecting within the admitted count.
- For every probe, use the top nine bits of the current probe hash as a bit
  address within the chosen 512-bit region, then multiply the hash by
  `0x9e3779b9` with uint32 wrapping.
- Bit numbering is low-bit-first within bytes, independent of host endianness.
- Probe count keeps the existing integer `bits_per_key * 69 / 100`, clamped
  to 1..30.
- Region count is `max(1, ceil(key_count * bits_per_key / 512))`.

These are compatibility identities, not tunable implementation details.
Different hashing, region mapping or bit progression needs a different
metadata name/version, even if its false-positive rate looks better.
Both scalar and accelerated readers must produce the same answers.

## Builder

Introduce a full-filter builder that collects fixed-size pairs of key hashes
while TableBuilder appends internal entries. Hash the complete user key;
include tombstones and every internal version. Duplicate user keys may set
the same bits and must never be dropped based on sequence visibility.

Collecting eight bytes per input key avoids keeping all key strings. It also
allows sizing the final region array once, rather than rebuilding it as data
blocks flush. Enforce representable output size before accepting another key.
After an allocation or callback exception, the enclosing table builder remains
unusable under its existing exception contract.

Keep the legacy builder unchanged. TableBuilder owns at most one active
filter builder. StartBlock applies only to the legacy layout. Finish writes
the selected payload as an ordinary uncompressed, CRC32C-protected metadata
block and records its handle under the selected metadata key.

Preserve error latching, append/Sync/Close ordering, last-key ordering,
compression selection and output cleanup. A new filter error must not
install a partially written SST.

## Reader and ownership

When filtering is enabled, read and validate the metaindex once. Prefer the
recognized full-table metadata key; otherwise use the existing legacy key.
An unknown key is ignored. A malformed recognized filter is Corruption, not a
quiet fallback to another filter or an empty result.
When a recognized full filter is selected, unselected legacy handles and
payloads are not separately decoded or read. Generic metaindex physical
validation still applies. Thus valid full metadata plus malformed legacy
payload is usable, while malformed selected full metadata plus valid legacy
payload is Corruption. An unknown full-version metadata name does not suppress
selection/validation of a recognized legacy filter.

Read the filter through existing stored-block CRC and ownership handling.
Before reading/allocating its payload, reject an inadmissible selected full
handle size. Version one has at most 67,108,863 regions; its payload is at
least 80 bytes, at most 4,294,967,248 bytes, and has the 16-plus-multiple-of-64
shape. File-region bounds and checked wide arithmetic still apply.
Admit the full layout once: magic, zero-reserved fields, probe domain,
nonzero region count, representable byte count and exact payload length.
Retain its BlockContents, preserving owned/vector/mmap-backed lifetimes and
move-stable backing addresses. The table's file owner outlives borrowed bytes.

Table Get tests the complete user key against the full filter before creating
or seeking an index iterator. A negative result is Missing, not a tombstone,
and permits looking in older candidate files. Positive results continue
through the unchanged index/data key, sequence and tombstone checks.
Legacy offset-range filtering remains after index seek when no full filter
was loaded. Iteration is not filtered.

Do not lazily introduce a scan or rebuild a filter in the reader.
This slice retains eager index loading/validation at Table Open; it avoids
repeated Get index seeks, not the first index read on every cold Open.
It does not change lazy data-entry corruption validation.

## Scalar and ISA boundary

First implement and review the scalar reader with exact format goldens.
Then add a separate AVX2 leaf over the same admitted 64-byte region.
The hot leaf accepts only a valid probe count and a complete region.
No recoverable bounds checks are repeated inside the probe loop.

Compile AVX2 only into its leaf translation unit. Do not change library-wide
architecture flags. Runtime selection must check CPU and OS YMM-state support
before executing it. Other architectures and unsupported x86 CPUs use scalar.
Select and retain the function pointer when admitting the reader, outside
the Get loop.

Use distinct namespaces/symbols for scalar and ISA-dependent definitions;
do not repeat differently conditioned inline definitions across translation
units. Inspect the linked functions, owners and actual instructions, not just
compiler flags or matching results. Both timed paths call the actual leaf
through the same one-indirect-call interface.

Implement the SIMD method as an equivalent bit test, not a new persisted
policy. Its loads stay within the admitted region, including unaligned data.

## Required evidence

- Literal header/bit goldens, not expectations regenerated by the production
  builder under test.
- Independent hash/bit oracle, all supported probe domains and region choices.
- Empty keys, binary keys, high-bit bytes, duplicate versions, tombstones,
  empty tables, small/large density and overflow boundaries.
- No false negatives for all inserted keys; false-positive evidence is
  diagnostic and not a correctness substitute.
- Owned and borrowed reader moves, all relevant backing alignments and exact
  final-region loads.
- Malformed magic/probes/reserved fields/counts/lengths and stored CRC errors,
  with explicit errors when the filter is configured.
- Mixed legacy/new/unfiltered files, enabled/disabled/configuration-change
  reopens and custom-comparator default/explicit behavior.
- Native reads of new metadata and Modern reads of native legacy filters;
  existing compression, mmap and copied-read paths.
- API validation and unchanged builder failure/Close behavior.
- Scalar/AVX2 equivalence and runtime-gating tests, plus linked-ISA proof on
  the measured MSVC build.
- Fresh fixed-work one/four-reader Get measurements with newly written
  filters, identical logical data, retained layout/provider controls,
  explicit filter/builder memory accounting and all original rounds.

## Review and delivery

This proposal must receive independent architecture review before production
edits. Resolve format, default-selection, lifetime and error-order findings.
Characterization, scalar format, writer/reader routing, public defaults and
ISA acceleration remain small commits. An independent implementation persona
reviews each complete bounded behavior change before publication.

Update [API and options](../reference/api-and-options.md),
[architecture](../architecture.md) and the
[tables/filter/cache lesson](../learning/05-tables-filters-and-caches.md)
when the behavior is implemented. Preserve ordinary compatibility,
coverage, sanitizer and CI gates.

The independent storage-architect review approved this bounded design.
Its two clarifications are incorporated above: selected handle size is admitted
before payload I/O/allocation, and full/legacy corruption precedence follows
explicit selection. This approval does not replace the independent
implementation review or the default-selection measurements.
