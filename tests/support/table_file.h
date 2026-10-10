#ifndef MODERN_LEVELDB_TESTS_SUPPORT_TABLE_FILE_H_
#define MODERN_LEVELDB_TESTS_SUPPORT_TABLE_FILE_H_

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/crc32c.h"
#include "table/block_builder.h"
#include "table/block_format.h"

namespace modern_leveldb::test_support {

inline std::vector<std::byte> EncodeBlock(
    std::initializer_list<std::pair<ByteView, ByteView>> entries) {
    BlockBuilder builder(1);
    for (const auto& [key, value] : entries) {
        const Status added = builder.Add(key, value);
        assert(added.has_value());
        static_cast<void>(added);
    }
    const ByteView encoded = builder.Finish();
    return {encoded.begin(), encoded.end()};
}

inline void AppendBlockEntry(std::vector<std::byte>& entries, std::uint32_t shared,
                             ByteView key_delta, ByteView value) {
    AppendVarint32(entries, shared);
    AppendVarint32(entries, static_cast<std::uint32_t>(key_delta.size()));
    AppendVarint32(entries, static_cast<std::uint32_t>(value.size()));
    entries.insert(entries.end(), key_delta.begin(), key_delta.end());
    entries.insert(entries.end(), value.begin(), value.end());
}

inline std::vector<std::byte> AddRestarts(std::vector<std::byte> entries,
                                          std::initializer_list<std::uint32_t> restarts) {
    for (const std::uint32_t restart : restarts) {
        AppendFixed32(entries, restart);
    }
    AppendFixed32(entries, static_cast<std::uint32_t>(restarts.size()));
    return entries;
}

inline std::vector<std::byte> EncodeHandle(BlockHandle handle) {
    std::vector<std::byte> encoded;
    AppendBlockHandle(encoded, handle);
    return encoded;
}

class TableFileAssembler final {
public:
    BlockHandle AddBlock(ByteView contents) {
        const BlockHandle handle{.offset = data_.size(), .size = contents.size()};
        data_.insert(data_.end(), contents.begin(), contents.end());
        data_.push_back(static_cast<std::byte>(BlockCompression::None));
        const ByteView typed = ByteView(data_).last(contents.size() + 1);
        AppendFixed32(data_, MaskCrc32c(Crc32c(typed)));
        return handle;
    }

    std::vector<std::byte> Finish(BlockHandle metaindex, BlockHandle index) {
        const auto footer = EncodeFooter({.metaindex = metaindex, .index = index});
        data_.insert(data_.end(), footer.begin(), footer.end());
        return std::move(data_);
    }

private:
    std::vector<std::byte> data_;
};

inline std::vector<std::byte> AssembleSingleDataBlockTable(ByteView data_contents,
                                                           ByteView index_key) {
    TableFileAssembler table;
    const BlockHandle data = table.AddBlock(data_contents);
    const std::vector<std::byte> handle = EncodeHandle(data);
    const BlockHandle index = table.AddBlock(EncodeBlock({{index_key, handle}}));
    return table.Finish(table.AddBlock(EncodeBlock({})), index);
}

}  // namespace modern_leveldb::test_support

#endif  // MODERN_LEVELDB_TESTS_SUPPORT_TABLE_FILE_H_
