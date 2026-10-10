#include <snappy.h>
#include <zstd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "format/internal_key.h"
#include "format/wal_format.h"
#include "format/write_batch.h"
#include "fuzz_support.h"
#include "metadata/version_edit.h"
#include "modern_leveldb/base/bytes.h"
#include "table/block.h"
#include "table/block_format.h"
#include "table/compression.h"

namespace modern_leveldb {
namespace {

using fuzz_support::Require;

bool WithinDecodeBudget(ByteView bytes, BlockCompression type) {
    // Reject advertised huge decode sizes so fuzzing focuses on parser behavior.
    // Unknown/malformed size metadata still reaches the decoder's own checks;
    // this prefilter is not a general bound on every allocation.
    constexpr std::size_t Limit = 1 << 20;
    if (type == BlockCompression::Snappy) {
        std::size_t length = 0;
        return !snappy::GetUncompressedLength(reinterpret_cast<const char*>(bytes.data()),
                                              bytes.size(), &length) ||
               length <= Limit;
    }
    if (type == BlockCompression::Zstd) {
        const auto length = ZSTD_getFrameContentSize(bytes.data(), bytes.size());
        return length == ZSTD_CONTENTSIZE_ERROR || length == ZSTD_CONTENTSIZE_UNKNOWN ||
               length <= Limit;
    }
    return true;
}

void FuzzFormat(ByteView input) {
    // Select one representation per input. Round-trip and traversal assertions
    // test internal consistency; independent golden bytes test format conventions.
    if (input.empty() || input.size() > 65536) {
        return;
    }
    const unsigned command = std::to_integer<unsigned>(input[0]);
    input = input.subspan(1);
    switch (command % 8) {
        case 0: {
            auto edit = VersionEdit::Decode(input);
            if (edit.has_value()) {
                const auto encoded = edit->Encode();
                auto again = VersionEdit::Decode(encoded);
                Require(again.has_value());
                Require(again->Encode() == encoded);
            }
            break;
        }
        case 1: {
            auto batch = WriteBatchReader::Open(input);
            if (batch.has_value()) {
                std::uint32_t count = 0;
                while (batch->Next().has_value()) {
                    Require(count < batch->count());
                    ++count;
                }
                Require(count == batch->count());
            }
            break;
        }
        case 2: {
            for (const bool verify : {false, true}) {
                const auto record = DecodeWalFragment(input, verify);
                if (record.has_value()) {
                    Require(record->encoded_size <= input.size());
                }
            }
            break;
        }
        case 3: {
            std::vector<std::byte> contents(input.begin(), input.end());
            auto block = Block::Create(std::move(contents));
            if (block.has_value()) {
                const bool physically_valid =
                    block->ValidateEntries([](ByteView, ByteView) -> Status { return {}; })
                        .has_value();
                const InternalKeyComparator internal(BytewiseComparator());
                const TrustedInternalKeyComparator trusted(internal);
                const bool internal_keys = (command & 8U) != 0U;
                const Comparator& comparator =
                    internal_keys ? static_cast<const Comparator&>(trusted) : BytewiseComparator();
                Block::Iterator iterator(
                    *block, comparator,
                    internal_keys ? BlockKeyFormat::Internal : BlockKeyFormat::Arbitrary);
                std::vector<std::pair<std::string, std::string>> forward;
                Status moved = iterator.SeekToFirst();
                while (moved.has_value() && iterator.valid()) {
                    Require(forward.size() <= input.size());
                    forward.emplace_back(AsStringView(iterator.key()),
                                         AsStringView(iterator.value()));
                    moved = iterator.Next();
                }
                if (physically_valid && moved.has_value()) {
                    moved = iterator.SeekToLast();
                    std::size_t count = forward.size();
                    while (moved.has_value() && iterator.valid()) {
                        Require(count != 0);
                        --count;
                        Require(AsStringView(iterator.key()) == forward[count].first);
                        Require(AsStringView(iterator.value()) == forward[count].second);
                        moved = iterator.Prev();
                    }
                    if (moved.has_value()) {
                        Require(count == 0);
                    }
                } else {
                    moved = iterator.SeekToLast();
                    std::size_t steps = 0;
                    while (moved.has_value() && iterator.valid()) {
                        Require(steps++ <= input.size());
                        moved = iterator.Prev();
                    }
                }
            }
            break;
        }
        case 4:
            if (input.size() >= FooterSize) {
                const auto footer = DecodeFooter(input.first<FooterSize>());
                if (footer.has_value()) {
                    const auto again = DecodeFooter(EncodeFooter(*footer));
                    Require(again.has_value());
                    Require(again->index.offset == footer->index.offset &&
                            again->index.size == footer->index.size &&
                            again->metaindex.offset == footer->metaindex.offset &&
                            again->metaindex.size == footer->metaindex.size);
                }
            }
            break;
        case 5: {
            const auto key = InternalKey::Decode(input);
            if (key.has_value()) {
                Require(std::ranges::equal(key->encoded(), input));
            }
            break;
        }
        case 6: {
            const auto type = static_cast<BlockCompression>((command >> 3U) % 4);
            if (WithinDecodeBudget(input, type)) {
                std::vector<std::byte> stored(input.begin(), input.end());
                const auto trailer = EncodeBlockTrailer(input, type);
                stored.insert(stored.end(), trailer.begin(), trailer.end());
                if ((command & 0x20U) == 0U) {
                    static_cast<void>(DecodeStoredBlock(std::move(stored)));
                } else {
                    static_cast<void>(DecodeStoredBlock(ByteView(stored)));
                }
            }
            break;
        }
        case 7: {
            const auto type =
                (command & 8U) == 0 ? BlockCompression::Snappy : BlockCompression::Zstd;
            std::vector<std::byte> compressed;
            if (TryCompressBlock(input, type, 1, compressed)) {
                const auto decoded = DecompressBlock(compressed, type);
                Require(decoded.has_value());
                Require(std::ranges::equal(*decoded, input));
            }
            break;
        }
    }
}

}  // namespace
}  // namespace modern_leveldb

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    modern_leveldb::FuzzFormat({reinterpret_cast<const std::byte*>(data), size});
    return 0;
}
