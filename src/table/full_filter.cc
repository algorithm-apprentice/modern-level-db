#include "table/full_filter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <utility>
#include <vector>

#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/hash.h"

namespace modern_leveldb {
namespace {

constexpr std::uint32_t Magic = 0x31464c4dU;
constexpr std::size_t HeaderBytes = 16;
constexpr std::size_t RegionBytes = 64;
constexpr std::uint64_t RegionBits = RegionBytes * 8;
constexpr std::uint64_t MaximumRegions =
    (std::numeric_limits<std::uint32_t>::max() - HeaderBytes) / RegionBytes;
constexpr std::uint64_t MaximumPayload = HeaderBytes + MaximumRegions * RegionBytes;
constexpr std::uint32_t BucketSeed = 0xbc9f1d34U;
constexpr std::uint32_t ProbeSeed = 0x9e3779b9U;
constexpr std::uint32_t ProbeMultiplier = 0x9e3779b9U;

std::size_t MaximumKeyCount(std::uint32_t bits_per_key) noexcept {
    if (bits_per_key == 0) {
        return std::numeric_limits<std::size_t>::max();
    }
    return static_cast<std::size_t>(std::min<std::uint64_t>(
        MaximumRegions * RegionBits / bits_per_key, std::numeric_limits<std::size_t>::max()));
}

std::uint32_t RegionOf(std::uint32_t hash, std::uint32_t regions) noexcept {
    return static_cast<std::uint32_t>((std::uint64_t{hash} * regions) >> 32U);
}

}  // namespace

bool IsFullFilterBlockSize(std::uint64_t size) noexcept {
    return size >= HeaderBytes + RegionBytes && size <= MaximumPayload &&
           (size - HeaderBytes) % RegionBytes == 0;
}

FullFilterBuilder::FullFilterBuilder(BloomFilterPolicy policy) noexcept
    : policy_(policy), maximum_keys_(MaximumKeyCount(policy.bits_per_key())) {}

Status FullFilterBuilder::AddKey(ByteView key) {
    if (hashes_.size() == maximum_keys_) {
        return std::unexpected(Error::InvalidArgument("full filter exceeds the uint32 size limit"));
    }
    hashes_.push_back({Hash32(key, BucketSeed), Hash32(key, ProbeSeed)});
    return {};
}

ByteView FullFilterBuilder::Finish() {
    const auto bits = std::uint64_t{hashes_.size()} * policy_.bits_per_key();
    const auto regions = static_cast<std::uint32_t>(
        std::max<std::uint64_t>(1, (bits + RegionBits - 1) / RegionBits));
    result_.assign(HeaderBytes + std::size_t{regions} * RegionBytes, std::byte{0});
    EncodeFixed32(MutableByteView(result_).first<4>(), Magic);
    result_[4] = static_cast<std::byte>(policy_.probes());
    EncodeFixed32(MutableByteView(result_).subspan<8, 4>(), regions);
    for (const auto& hash : hashes_) {
        const auto start = HeaderBytes + std::size_t{RegionOf(hash.bucket, regions)} * RegionBytes;
        auto value = hash.probes;
        for (std::uint32_t probe = 0; probe < policy_.probes(); ++probe) {
            const auto bit = value >> 23U;
            result_[start + bit / 8] |= static_cast<std::byte>(1U << (bit % 8));
            value *= ProbeMultiplier;
        }
    }
    return result_;
}

Result<FullFilterReader> FullFilterReader::Create(std::vector<std::byte> contents) {
    return Create(BlockContents::Owned(std::move(contents)));
}

Result<FullFilterReader> FullFilterReader::Create(BlockContents contents) {
    const ByteView data = contents.data();
    if (!IsFullFilterBlockSize(data.size())) {
        return std::unexpected(Error::Corruption("full filter size is invalid"));
    }
    if (DecodeFixed32(data.first<4>()) != Magic) {
        return std::unexpected(Error::Corruption("full filter version is invalid"));
    }
    if (data[5] != std::byte{0} || data[6] != std::byte{0} || data[7] != std::byte{0} ||
        DecodeFixed32(data.subspan<12, 4>()) != 0) {
        return std::unexpected(Error::Corruption("full filter reserved fields are not zero"));
    }
    const auto probes = std::to_integer<std::uint32_t>(data[4]);
    if (probes == 0 || probes > 30) {
        return std::unexpected(Error::Corruption("full filter probe count is invalid"));
    }
    const auto regions = DecodeFixed32(data.subspan<8, 4>());
    if (regions == 0 || std::uint64_t{regions} * RegionBytes + HeaderBytes != data.size()) {
        return std::unexpected(Error::Corruption("full filter region count is invalid"));
    }
    return FullFilterReader(std::move(contents), regions, probes);
}

FullFilterReader::FullFilterReader(BlockContents contents, std::uint32_t regions,
                                   std::uint32_t probes) noexcept
    : contents_(std::move(contents)),
      regions_(regions),
      probes_(probes),
      probe_(full_filter_detail::RuntimeProbe()) {}

bool FullFilterReader::KeyMayMatch(ByteView user_key) const noexcept {
    const auto region = RegionOf(Hash32(user_key, BucketSeed), regions_);
    const auto* data = contents_.data().data() + HeaderBytes + std::size_t{region} * RegionBytes;
    return probe_(Hash32(user_key, ProbeSeed), probes_, data);
}

}  // namespace modern_leveldb
