#include "table/bloom_filter.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/hash.h"

namespace modern_leveldb {
namespace {

constexpr std::uint32_t HashSeed = 0xbc9f1d34;
constexpr std::uint32_t MaximumProbes = 30;
constexpr std::uint64_t MinimumBits = 64;
constexpr std::uint64_t MaximumBits = std::numeric_limits<std::uint64_t>::max();

std::uint32_t BloomHash(ByteView key) noexcept { return Hash32(key, HashSeed); }

std::byte BitMask(std::uint64_t position) noexcept {
  return static_cast<std::byte>(1U << (position % 8));
}

}  // namespace

BloomFilterPolicy::BloomFilterPolicy(std::uint32_t bits_per_key) noexcept
    : bits_per_key_(bits_per_key),
      // LevelDB rounds bits_per_key * ln(2), approximated as 0.69, down.
      probes_(static_cast<std::uint32_t>(
          std::clamp<std::uint64_t>(std::uint64_t{bits_per_key} * 69 / 100, 1, MaximumProbes))) {}

std::uint64_t BloomFilterPolicy::FilterSize(std::size_t key_count) const noexcept {
  const std::uint64_t count = key_count;
  // Saturate instead of wrapping, so that an impossible size stays impossible.
  const std::uint64_t bits = bits_per_key_ != 0 && count > MaximumBits / bits_per_key_
                                 ? MaximumBits
                                 : std::max(MinimumBits, count * bits_per_key_);
  // Whole bytes of bits, then the probe count.
  return (bits - 1) / 8 + 2;
}

void BloomFilterPolicy::CreateFilter(std::span<const ByteView> keys,
                                     std::vector<std::byte>& output) const {
  // A size that does not fit in memory saturates, so that insert throws.
  const auto bytes = static_cast<std::size_t>(std::min<std::uint64_t>(
      FilterSize(keys.size()) - 1, std::numeric_limits<std::size_t>::max()));
  const std::size_t start = output.size();
  output.insert(output.end(), bytes, std::byte{0});
  output.push_back(static_cast<std::byte>(probes_));
  const std::uint64_t bits = std::uint64_t{bytes} * 8;

  const MutableByteView filter = MutableByteView(output).subspan(start, bytes);
  for (const ByteView key : keys) {
    std::uint32_t hash = BloomHash(key);
    const std::uint32_t delta = std::rotr(hash, 17);
    for (std::uint32_t probe = 0; probe < probes_; ++probe) {
      const std::uint64_t position = hash % bits;
      filter[static_cast<std::size_t>(position / 8)] |= BitMask(position);
      hash += delta;
    }
  }
}

bool BloomFilterPolicy::KeyMayMatch(ByteView key, ByteView filter) const noexcept {
  if (filter.size() < 2) {
    return false;
  }
  const std::uint64_t bits = std::uint64_t{filter.size() - 1} * 8;
  const auto probes = std::to_integer<std::uint32_t>(filter.back());
  if (probes > MaximumProbes) {
    // LevelDB reserves larger probe counts for future filter encodings.
    return true;
  }

  std::uint32_t hash = BloomHash(key);
  const std::uint32_t delta = std::rotr(hash, 17);
  for (std::uint32_t probe = 0; probe < probes; ++probe) {
    const std::uint64_t position = hash % bits;
    if ((filter[static_cast<std::size_t>(position / 8)] & BitMask(position)) == std::byte{0}) {
      return false;
    }
    hash += delta;
  }
  return true;
}

}  // namespace modern_leveldb
