# ADR-0007: Checksum and Hash Contracts

- Status: Accepted
- Date: 2026-09-21

## Context

The initial checksum implementation is extended by
[ADR-0044](0044-profile-guided-crc32c-acceleration.md), which preserves these
public and persistent contracts while adopting runtime-dispatched CRC32C.

LevelDB WAL records and SSTable blocks use masked CRC32C checksums. Its Bloom
filters use a seeded 32-bit hash whose output must remain stable to read
existing filter data. Substituting ordinary CRC32, `std::hash`, or a different
Murmur-family hash would break compatibility.

This implements only the `implement-checksum-hash` DAG node. WAL, SSTable,
cache, and filter implementations remain separate future nodes.

## Decision

Provide these allocation-free functions in the `modern_leveldb` namespace:

| API | Contract |
|---|---|
| `Crc32c(ByteView)` | Compute an unmasked CRC32C; empty input returns zero. |
| `ExtendCrc32c(std::uint32_t crc, ByteView)` | Continue an unmasked, finalized CRC over another byte sequence. Empty input preserves `crc`. |
| `MaskCrc32c(std::uint32_t crc)` | Apply LevelDB's reversible transform for stored checksums. |
| `UnmaskCrc32c(std::uint32_t masked_crc)` | Recover the original checksum from its masked representation. |
| `Hash32(ByteView, std::uint32_t seed)` | Compute the exact LevelDB seeded 32-bit hash; empty input returns the seed. |

All functions return `std::uint32_t`, are `[[nodiscard]]` and `noexcept`, and
accept borrowed byte views by value. They do not consume the caller's view or
modify its storage. A valid view may be empty or unaligned and may contain
arbitrary bytes, including zeros and values above `0x7f`.

These functions have no normal failure result, so they do not return `Result`.
Neither CRC32C nor the hash provides cryptographic integrity or authentication.

### CRC32C

Use the Castagnoli polynomial, represented as `0x82f63b78` for reflected
processing, with an internal initial/final XOR of `0xffffffff`.

`ExtendCrc32c` accepts the finalized checksum, not the internal running register
or a masked on-disk checksum. It must satisfy:

```text
ExtendCrc32c(Crc32c(A), B) == Crc32c(A || B)
```

Use a compile-time-generated, 256-entry lookup table as the portable baseline.
Do not add CPU-specific instructions, runtime dispatch, or optional checksum
dependencies in this node.

### Checksum masking

Use LevelDB's exact transform, with arithmetic modulo 2^32:

```text
MaskCrc32c(crc) = rotr(crc, 15) + 0xa282ead8
UnmaskCrc32c(masked_crc) = rotl(masked_crc - 0xa282ead8, 15)
```

Masking is a storage-format transform, not encryption. Serialization of the
result into WAL or SSTable bytes belongs to later format nodes.

### Seeded hash

Use the LevelDB algorithm, not a generic implementation of MurmurHash:

- Initialize from the seed XOR the input length multiplied by `0xc6a4a793`,
  retaining the low 32 bits.
- Read full four-byte words in little-endian order through the existing
  fixed-width decoder.
- For each full word, add it, multiply by the same constant, then XOR with a
  right shift of 16.
- If one to three bytes remain, add their little-endian value, multiply, then
  XOR with a right shift of 24.

All arithmetic is unsigned modulo 2^32. The caller supplies the seed
explicitly; this module does not choose Bloom-filter or cache policy.

## Validation

Tests use RFC 3720 checksum vectors and independently generated results from
unmodified Google LevelDB revision
`7ee830d02b623e8ffe0b95d59a74db1e58da04c5`.

The reference hash inputs are every prefix of this binary sequence, with seeds
`0`, `0xbc9f1d34`, and `0x12345678`:

```text
ff 00 80 7f 01 fe 02 81 03 fd 04 82
```

Coverage includes empty input, all partial-word lengths, unsigned byte
handling, unaligned views, incremental CRC updates, independent masking and
unmasking vectors, and caller-view preservation. Keep deterministic unit
inputs small; the normal test suite must not build or download upstream
LevelDB.

Future optimized paths require the same compatibility vectors and measured
performance evidence before replacing this portable baseline.

## References

- [LevelDB checksum interface](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/util/crc32c.h)
- [LevelDB hash implementation](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/util/hash.cc)
- [RFC 3720, Appendix B](https://www.rfc-editor.org/rfc/rfc3720#appendix-B)
