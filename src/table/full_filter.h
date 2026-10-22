#ifndef MODERN_LEVELDB_TABLE_FULL_FILTER_H_
#define MODERN_LEVELDB_TABLE_FULL_FILTER_H_

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"
#include "table/block_format.h"
#include "table/bloom_filter.h"
#include "table/full_filter_probe.h"

namespace modern_leveldb {

inline constexpr std::string_view FullFilterMetaKey = "filter.modern_leveldb.FullBloom1";

// Size-only admission precedes any selected filter payload read or allocation.
[[nodiscard]] bool IsFullFilterBlockSize(std::uint64_t size) noexcept;

// Collects complete user-key hashes for one immutable table. Finish is called
// once; its view stays valid until destruction. Do not reuse after an exception.
class FullFilterBuilder final {
public:
    explicit FullFilterBuilder(BloomFilterPolicy policy) noexcept;
    FullFilterBuilder(const FullFilterBuilder&) = delete;
    FullFilterBuilder& operator=(const FullFilterBuilder&) = delete;
    FullFilterBuilder(FullFilterBuilder&&) = delete;
    FullFilterBuilder& operator=(FullFilterBuilder&&) = delete;
    ~FullFilterBuilder() = default;

    // Output-size rejection leaves the collected hashes unchanged.
    [[nodiscard]] Status AddKey(ByteView key);
    [[nodiscard]] ByteView Finish();
    [[nodiscard]] std::size_t key_count() const noexcept { return hashes_.size(); }

private:
    struct HashPair {
        std::uint32_t bucket;
        std::uint32_t probes;
    };

    BloomFilterPolicy policy_;
    std::size_t maximum_keys_;
    std::vector<HashPair> hashes_;
    std::vector<std::byte> result_;
};

// A validated, versioned full-table filter. BlockContents owns backing storage
// or borrows immutable file bytes; a borrowing reader must not outlive that file.
class FullFilterReader final {
public:
    [[nodiscard]] static Result<FullFilterReader> Create(std::vector<std::byte> contents);
    [[nodiscard]] static Result<FullFilterReader> Create(BlockContents contents);

    FullFilterReader(const FullFilterReader&) = delete;
    FullFilterReader& operator=(const FullFilterReader&) = delete;
    FullFilterReader(FullFilterReader&&) noexcept = default;
    FullFilterReader& operator=(FullFilterReader&&) = delete;
    ~FullFilterReader() = default;

    [[nodiscard]] bool KeyMayMatch(ByteView user_key) const noexcept;

private:
    FullFilterReader(BlockContents contents, std::uint32_t regions, std::uint32_t probes) noexcept;

    BlockContents contents_;
    std::uint32_t regions_;
    std::uint32_t probes_;
    full_filter_detail::Probe probe_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_TABLE_FULL_FILTER_H_
