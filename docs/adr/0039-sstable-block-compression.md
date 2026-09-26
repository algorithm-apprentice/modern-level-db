# ADR-0039: SSTable Block Compression

## Status

Accepted

## Context

LevelDB SSTable blocks already carry the persistent five-byte trailer defined
by ADR-0022: one compression byte followed by the masked CRC32C of the stored
block bytes and that byte. Modern LevelDB currently writes type `0` and
returns `NotSupported` for the LevelDB-compatible Snappy (`1`) and Zstd (`2`)
types.

The `implement-compression` DAG node must read the default Snappy tables that
Google LevelDB writes, offer Zstd where LevelDB does, and write tables that
LevelDB can read. The change must preserve the existing block, table, cache,
engine, and public API boundaries.

Google LevelDB compresses each block independently. It stores a compressed
block only when the compressed bytes are strictly smaller than
`raw_size - raw_size / 8`; otherwise it writes the original bytes with type
`0`. This 12.5% threshold avoids spending read CPU for negligible space
savings. It uses raw Snappy blocks and self-describing Zstd frames.

RocksDB adds compression dictionaries, per-level policies, parallel
compression, checksums beyond the block trailer, and additional algorithms.
No current caller or format commitment requires those features.

## Decision

### Dependencies

Use the official libraries:

- Google Snappy `1.3.1`.
- Meta Zstandard `1.5.7`.

CMake first reuses an existing parent target:

- `Snappy::snappy` or `snappy`;
- `zstd::libzstd_static`, `zstd::libzstd_shared`,
  `libzstd_static`, `libzstd_shared`, or `libzstd`.

If no suitable target exists, `FetchContent` obtains the pinned release.
Dependency tests, benchmarks, programs, installation, legacy Zstd decoders,
dictionary builders, and Zstd multithreading default off when the parent has
not already chosen those options. Existing parent values are never forced.
The Zstd fallback configures the release's `build/cmake` source directory;
the archive root is not a CMake project. Fallback subdirectories are excluded
from the default build and installation, while linking their selected static
targets still builds the libraries.

Snappy declares the generic `BUILD_SHARED_LIBS` cache variable when it is
absent, and Zstd force-sets the generic `CMAKE_BUILD_TYPE` cache variable to
`Release` for an otherwise unconfigured single-configuration generator.
Modern LevelDB snapshots whether those variables exist and their values before
configuring a fallback, then restores or removes them exactly afterward.
Consumer tests cover both initially undefined variables and parent-selected
values. Codec-specific options follow the same set-only-when-undefined rule.
The compression libraries are private implementation dependencies; no public
header includes them.

### Internal compression contract

Add `src/table/compression.{h,cc}`:

```cpp
enum class BlockCompression : std::uint8_t {
  None = 0,
  Snappy = 1,
  Zstd = 2,
};

bool TryCompressBlock(ByteView raw, BlockCompression requested,
                      int zstd_level, std::vector<std::byte>& scratch);

Result<std::vector<std::byte>> DecompressBlock(
    ByteView stored, BlockCompression type);
```

`TryCompressBlock` reuses `scratch` across calls and returns true only when it
contains a compressed representation that beats LevelDB's 12.5% threshold.
It returns false for `None`, for a library compression failure, or when the
threshold is not met; the caller then writes the raw block with type `None`.
A library failure is therefore a performance degradation, not data loss,
matching LevelDB. The caller validates the enum and Zstd level before any
file write.

Snappy uses `MaxCompressedLength`, `RawCompress`,
`GetUncompressedLength`, and `RawUncompress`. Zstd uses
`ZSTD_compressBound`, `ZSTD_compress`, `ZSTD_getFrameContentSize`, and
`ZSTD_decompress`.

Decompression:

1. validates the trailer checksum over the stored bytes and type before
   interpreting the compressed stream;
2. obtains the declared uncompressed size;
3. rejects an invalid or unknown size, a zero-sized Zstd frame, a size above
   `UINT32_MAX`, or a size that cannot fit in `std::vector<std::byte>`;
4. allocates exactly that size;
5. requires decompression to succeed and produce exactly that many bytes.

An empty raw Snappy stream is valid and decodes to an empty vector. Empty input
requested for writing cannot meet the compression threshold and is stored as
type `None`. A Zstd frame declaring zero output is corrupt, matching Google
LevelDB's reader.

Malformed compressed bytes return `Corruption`. An unknown trailer type
returns `Corruption`. Allocation failures continue to throw as the rest of
the table reader does.

### Stored-block format

`EncodeBlockTrailer` accepts the actual `BlockCompression`. Its checksum is
computed over the bytes written to the file, compressed or raw, followed by
the type byte.

`DecodeStoredBlock` keeps the existing zero-copy resize for type `None`.
For Snappy and Zstd it returns a newly allocated uncompressed vector.

A `BlockHandle::size` remains the number of stored bytes before the trailer.
The block cache charges the decoded `Block` content size, as LevelDB does,
while the handle continues to describe the compressed on-disk bytes. Charging
the stored size would let highly compressible data exceed the cache's memory
budget by an arbitrary compression ratio.

### Table writer and reader

Extend `TableBuilderOptions`:

```cpp
BlockCompression compression = BlockCompression::None;
int zstd_compression_level = 1;
```

`TableBuilder::WriteBlock` tries the configured compression for every data,
metaindex, and index block, then writes either the compressed result and its
type or the raw bytes and type `None`. It sets each handle to the stored size
and reuses one scratch vector across blocks. Filter blocks always use
`WriteRawBlock` with type `None`, matching Google LevelDB's format policy.

Zstd levels outside LevelDB's supported range `[-5, 22]` make the builder
return `InvalidArgument` without writing. Unknown internal compression enum
values do the same.

The reader needs no option to decode: each block's trailer selects the
algorithm. Existing table lookup and iteration paths receive uncompressed
bytes from `DecodeStoredBlock`.

### Public API

Extend `modern_leveldb/options.h`:

```cpp
enum class Compression {
  None,
  Snappy,
  Zstd,
};

struct Options {
  // Existing fields...
  Compression compression = Compression::Snappy;
  int zstd_compression_level = 1;
};
```

Snappy becomes the public default, matching Google LevelDB. `None` preserves
the exact uncompressed files used by earlier unit and golden tests. `Zstd`
uses the configured level. `Database::Open` rejects an unknown enum or a Zstd
level outside `[-5, 22]` before creating a directory.

Private `DatabaseEngineOptions` retain `BlockCompression::None` as their
default so low-level tests and internal callers must opt into compression
explicitly. The public facade maps its option into the engine.

## Rejected alternatives

- **Implement the codecs locally.** Mature, heavily tested libraries already
  define these formats; a new implementation would add security and
  compatibility risk without product value.
- **Require system packages only.** That would add manual setup to every CI
  platform and make ordinary CMake consumers less reproducible.
- **Always vendor or force dependency settings.** That would prevent parent
  projects from supplying approved targets and repeat the embedding problems
  fixed in the bootstrap node.
- **Compress filter blocks.** Google LevelDB deliberately writes filter blocks
  with type `None`; Modern LevelDB follows that policy while compressing data,
  metaindex, and index blocks.
- **Always store compressed output.** LevelDB's threshold is part of its
  proven latency/space tradeoff and avoids expansion on small or random data.
- **Streaming decompression, dictionaries, custom codecs, or per-level
  policies.** Current blocks are bounded below 4 GiB, and no caller requires
  those additional contracts.

## Validation

Unit and format tests cover:

- The persistent type values `0`, `1`, and `2` and checksums over stored bytes.
- Snappy and Zstd round trips for small, compressible, binary, and maximum
  practical test inputs; an empty requested compression falling back to
  `None`; an empty Snappy stream decoding successfully; and a zero-content
  Zstd frame returning `Corruption`.
- Fallback to type `None` at and above the 87.5% size threshold.
- Independent Snappy and Zstd golden streams produced by unmodified Google
  LevelDB and decoded by Modern LevelDB.
- Truncated, malformed, unknown-size, checksum-damaged, and oversized
  compressed blocks.
- Tables whose data blocks use Snappy and Zstd, including point reads,
  forward/reverse iteration, filters, and block caching. Tests assert that
  filter trailers stay uncompressed and compressed data blocks charge their
  decoded size to the cache.
- Public `None`, Snappy-default, and Zstd databases reopening through both
  Modern LevelDB and Google LevelDB.
- Rejection of unknown public compression values and invalid Zstd levels
  before filesystem mutation.
- CMake consumers reusing parent dependency targets and default dependency
  builds that add no dependency tests, programs, benchmarks, or install
  targets, locate Zstd below `build/cmake`, and do not introduce or replace
  the parent's `BUILD_SHARED_LIBS` or `CMAKE_BUILD_TYPE` values.

An isolated differential helper has both implementations write randomized
tables and databases with each compression mode, then opens and scans the
other implementation's output.

The existing Debug, Release, sanitizer, coverage, and platform CI matrix
remains required.

## Consequences

- Modern LevelDB reads Google LevelDB's default tables and writes compressed
  tables that Google LevelDB reads.
- Public databases now default to Snappy and can select uncompressed or Zstd
  blocks.
- Library builds acquire two private codec dependencies unless the parent
  already provides suitable targets.
- Compression remains a block-format policy; engine and metadata layers do
  not call codec libraries directly.

## References

- [Google LevelDB table builder](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/table/table_builder.cc)
- [Google LevelDB table block reader](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/table/format.cc)
- [Google Snappy 1.3.1](https://github.com/google/snappy/releases/tag/1.3.1)
- [Zstandard 1.5.7](https://github.com/facebook/zstd/releases/tag/v1.5.7)
