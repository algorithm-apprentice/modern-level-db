#include "diagnostics/dump_file.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "format/internal_key.h"
#include "format/write_batch.h"
#include "metadata/version_edit.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"
#include "support/memory_file_system.h"
#include "support/table_file.h"
#include "table/block.h"
#include "table/block_format.h"
#include "table/bloom_filter.h"
#include "table/compression.h"
#include "table/table_builder.h"
#include "wal/wal_io.h"

namespace modern_leveldb {
namespace {

using test_support::AssembleSingleDataBlockTable;
using test_support::EncodeBlock;
using test_support::EncodeHandle;
using test_support::MemoryFileSystem;
using test_support::TableFileAssembler;

class StringOutput final : public WritableFile {
public:
    explicit StringOutput(std::optional<std::size_t> fail_at = std::nullopt) : fail_at_(fail_at) {}

    Status Append(ByteView data) override {
        if (fail_at_ == appends_.size()) {
            return std::unexpected(Error::Io("injected output failure"));
        }
        appends_.emplace_back(AsStringView(data));
        return {};
    }
    Status Flush() override { return {}; }
    Status Sync() override { return {}; }
    Status Close() override { return {}; }

    [[nodiscard]] std::string text() const {
        std::string result;
        for (const std::string& append : appends_) {
            result += append;
        }
        return result;
    }

    [[nodiscard]] const std::vector<std::string>& appends() const noexcept { return appends_; }

private:
    std::optional<std::size_t> fail_at_;
    std::vector<std::string> appends_;
};

std::vector<std::byte> Bytes(std::initializer_list<unsigned int> values) {
    std::vector<std::byte> bytes;
    for (const unsigned int value : values) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

std::vector<std::byte> Materialize(ByteView bytes) {
    return std::vector<std::byte>(bytes.begin(), bytes.end());
}

InternalKey Key(ByteView user_key, SequenceNumber sequence, ValueKind kind = ValueKind::Value) {
    Result<InternalKey> key = InternalKey::Create(user_key, sequence, kind);
    EXPECT_TRUE(key.has_value());
    return std::move(*key);
}

void WriteRecords(MemoryFileSystem& file_system, const std::filesystem::path& path,
                  const std::vector<std::vector<std::byte>>& records) {
    Result<std::unique_ptr<WritableFile>> file = file_system.OpenWritable(path);
    ASSERT_TRUE(file.has_value());
    WalWriter writer(std::move(*file));
    for (const std::vector<std::byte>& record : records) {
        ASSERT_TRUE(writer.AddRecord(record).has_value());
    }
    ASSERT_TRUE(writer.Close().has_value());
}

std::vector<std::byte> Batch(SequenceNumber sequence, ByteView key, ByteView value) {
    EncodedWriteBatch batch;
    batch.Put(key, value);
    batch.SetSequence(sequence);
    return Materialize(batch.encoded());
}

class ReverseComparator final : public Comparator {
public:
    int Compare(ByteView left, ByteView right) const noexcept override {
        return -BytewiseComparator().Compare(left, right);
    }
    std::string_view Name() const noexcept override { return "test.Reverse"; }
    void FindShortestSeparator(std::vector<std::byte>&, ByteView) const override {}
    void FindShortSuccessor(std::vector<std::byte>&) const override {}
};

void BuildTable(MemoryFileSystem& file_system, const std::filesystem::path& path,
                const Comparator& user_comparator, BlockCompression compression,
                std::optional<BloomFilterPolicy> filter = std::nullopt) {
    Result<std::unique_ptr<WritableFile>> file = file_system.OpenWritable(path);
    ASSERT_TRUE(file.has_value());
    const InternalKeyComparator comparator(user_comparator);
    TableBuilderOptions options;
    options.block_size = 1;
    options.compression = compression;
    options.filter_policy = std::move(filter);
    TableBuilder builder(std::move(*file), comparator, options);
    if (user_comparator.Name() == "test.Reverse") {
        const InternalKey b = Key(AsBytes("b"), 3);
        const InternalKey a = Key(AsBytes("a"), 2, ValueKind::Deletion);
        ASSERT_TRUE(builder.Add(b.encoded(), AsBytes("two")).has_value());
        ASSERT_TRUE(builder.Add(a.encoded(), AsBytes("unexpected")).has_value());
    } else {
        const InternalKey first = Key(AsBytes("a"), 3);
        const InternalKey second = Key(AsBytes("a"), 2, ValueKind::Deletion);
        const InternalKey third = Key(Bytes({'b', 0, 0xff}), 1);
        ASSERT_TRUE(builder.Add(first.encoded(), AsBytes("one")).has_value());
        ASSERT_TRUE(builder.Add(second.encoded(), AsBytes("unexpected")).has_value());
        ASSERT_TRUE(builder.Add(third.encoded(), Bytes({'v', '\n'})).has_value());
    }
    ASSERT_TRUE(builder.Finish().has_value());
}

std::uint64_t FilterBlockOffset(const std::vector<std::byte>& table) {
    const ByteView encoded(table);
    const Result<Footer> footer = DecodeFooter(
        std::span<const std::byte, FooterSize>(encoded.last<FooterSize>().data(), FooterSize));
    EXPECT_TRUE(footer.has_value());
    const BlockHandle metaindex = footer->metaindex;
    const ByteView stored =
        encoded.subspan(static_cast<std::size_t>(metaindex.offset),
                        static_cast<std::size_t>(metaindex.size) + BlockTrailerSize);
    Result<BlockContents> contents = DecodeStoredBlock(stored);
    EXPECT_TRUE(contents.has_value());
    Result<Block> block = Block::Create(std::move(*contents));
    EXPECT_TRUE(block.has_value());
    Block::Iterator iterator(*block, BytewiseComparator());
    EXPECT_TRUE(iterator.SeekToFirst().has_value());
    EXPECT_TRUE(iterator.valid());
    ByteView value = iterator.value();
    const Result<BlockHandle> filter = ConsumeBlockHandle(value);
    EXPECT_TRUE(filter.has_value());
    EXPECT_TRUE(value.empty());
    return filter->offset;
}

void PopulateEveryManifestField(VersionEdit& edit) {
    edit.SetComparatorName("cmp'\n");
    edit.SetLogNumber(2);
    edit.SetPrevLogNumber(1);
    ASSERT_TRUE(edit.SetNextFileNumber(7).has_value());
    ASSERT_TRUE(edit.SetLastSequence(41).has_value());
    ASSERT_TRUE(edit.AddCompactPointer(1, Key(AsBytes("m"), MaxSequenceNumber)).has_value());
    ASSERT_TRUE(edit.RemoveFile(2, 9).has_value());
    ASSERT_TRUE(edit.RemoveFile(1, 8).has_value());
    FileMetadata file{.number = 5,
                      .file_size = 4096,
                      .smallest = Key(AsBytes("a"), 3, ValueKind::Deletion),
                      .largest = Key(AsBytes("z"), 1)};
    ASSERT_TRUE(edit.AddFile(2, std::move(file)).has_value());
}

struct TwoBlockTable {
    std::vector<std::byte> contents;
    std::uint64_t second_offset;
};

TwoBlockTable BadKeyThenValidTable() {
    TableFileAssembler table;
    const BlockHandle bad_key = table.AddBlock(EncodeBlock({{AsBytes("bad"), AsBytes("first")}}));
    const InternalKey valid_key = Key(AsBytes("z"), 1);
    const BlockHandle second =
        table.AddBlock(EncodeBlock({{valid_key.encoded(), AsBytes("second")}}));
    const std::vector<std::byte> first_handle = EncodeHandle(bad_key);
    const std::vector<std::byte> second_handle = EncodeHandle(second);
    const InternalKey first_index = Key(AsBytes("m"), MaxSequenceNumber);
    const InternalKey second_index = Key(AsBytes("z"), MaxSequenceNumber);
    const BlockHandle index = table.AddBlock(EncodeBlock(
        {{first_index.encoded(), first_handle}, {second_index.encoded(), second_handle}}));
    return {
        .contents = table.Finish(table.AddBlock(EncodeBlock({})), index),
        .second_offset = second.offset,
    };
}

TEST(DumpFileTest, DumpsWriteBatchesWithEscapedBinaryDataAndPerEntrySequences) {
    MemoryFileSystem file_system;
    const std::filesystem::path path = "db/000001.log";
    EncodedWriteBatch batch;
    batch.Put(Bytes({'a', '\'', '\\', '\n', 0, 0xff}), Bytes({'v', '\r', '\t'}));
    batch.Delete(AsBytes("old"));
    batch.SetSequence(40);
    WriteRecords(file_system, path, {Materialize(batch.encoded())});
    StringOutput output;

    const Status dumped = DumpFile(file_system, path, output);

    ASSERT_TRUE(dumped.has_value()) << dumped.error().ToString();
    EXPECT_EQ(output.text(),
              "dump version=1 type=log file='db/000001.log'\n"
              "record offset=0 sequence=40 count=2\n"
              "  put sequence=40 key='a\\'\\\\\\n\\x00\\xff' value='v\\r\\t'\n"
              "  delete sequence=41 key='old'\n");
    ASSERT_EQ(output.appends().size(), 4U);
    for (const std::string& append : output.appends()) {
        EXPECT_TRUE(append.ends_with('\n'));
    }
}

TEST(DumpFileTest, PrintsPhysicalCorruptionContinuesAtTheNextBlockAndReturnsIt) {
    MemoryFileSystem file_system;
    const std::filesystem::path path = "000002.log";
    std::vector<std::byte> full_block_payload(WalBlockSize - WalHeaderSize, std::byte{'x'});
    const std::vector<std::byte> valid = Batch(7, AsBytes("good"), AsBytes("value"));
    WriteRecords(file_system, path, {full_block_payload, valid});
    std::vector<std::byte> corrupted = *file_system.Contents(path);
    corrupted[0] ^= std::byte{0x01};
    file_system.Write(path, std::move(corrupted));
    StringOutput output;

    const Status dumped = DumpFile(file_system, path, output);

    ASSERT_FALSE(dumped.has_value());
    EXPECT_EQ(dumped.error().code(), ErrorCode::Corruption);
    EXPECT_NE(output.text().find(
                  "corruption offset=0 dropped_bytes=32768 error='corruption: WAL physical record "
                  "checksum mismatch'\n"),
              std::string::npos);
    EXPECT_NE(output.text().find("record offset=32768 sequence=7 count=1\n"), std::string::npos);
}

TEST(DumpFileTest, PrintsMalformedBatchContinuesAndReturnsCorruption) {
    MemoryFileSystem file_system;
    const std::filesystem::path path = "000003.log";
    WriteRecords(
        file_system, path,
        {Bytes({0x01, 0x02}), Bytes({0x03, 0x04}), Batch(9, AsBytes("after"), AsBytes("ok"))});
    StringOutput output;

    const Status dumped = DumpFile(file_system, path, output);

    ASSERT_FALSE(dumped.has_value());
    EXPECT_EQ(dumped.error().code(), ErrorCode::Corruption);
    EXPECT_NE(output.text().find("record_error offset=0 error='corruption:"), std::string::npos);
    EXPECT_NE(output.text().find("record offset=18 sequence=9 count=1\n"), std::string::npos);
}

TEST(DumpFileTest, TreatsATruncatedFinalFragmentAsBenignEndOfFile) {
    MemoryFileSystem file_system;
    const std::filesystem::path path = "000004.log";
    EncodedWriteBatch large;
    const std::vector<std::byte> value(40000, std::byte{'v'});
    large.Put(AsBytes("large"), value);
    large.SetSequence(2);
    WriteRecords(file_system, path,
                 {Batch(1, AsBytes("kept"), AsBytes("one")), Materialize(large.encoded())});
    std::vector<std::byte> truncated = *file_system.Contents(path);
    truncated.resize(truncated.size() - 10);
    file_system.Write(path, std::move(truncated));
    StringOutput output;

    const Status dumped = DumpFile(file_system, path, output);

    ASSERT_TRUE(dumped.has_value()) << dumped.error().ToString();
    EXPECT_NE(output.text().find("record offset=0 sequence=1 count=1\n"), std::string::npos);
    EXPECT_EQ(output.text().find("sequence=2"), std::string::npos);
}

TEST(DumpFileTest, DumpsEveryManifestFieldInCanonicalContainerOrder) {
    MemoryFileSystem file_system;
    const std::filesystem::path path = "MANIFEST-000005";
    VersionEdit edit;
    PopulateEveryManifestField(edit);
    WriteRecords(file_system, path, {edit.Encode()});
    StringOutput output;

    const Status dumped = DumpFile(file_system, path, output);

    ASSERT_TRUE(dumped.has_value()) << dumped.error().ToString();
    EXPECT_EQ(output.text(),
              "dump version=1 type=manifest file='MANIFEST-000005'\n"
              "record offset=0\n"
              "  comparator name='cmp\\'\\n'\n"
              "  log_number value=2\n"
              "  previous_log_number value=1\n"
              "  next_file_number value=7\n"
              "  last_sequence value=41\n"
              "  compact_pointer level=1 key={user_key='m' sequence=72057594037927935 "
              "kind=value}\n"
              "  delete_file level=1 number=8\n"
              "  delete_file level=2 number=9\n"
              "  add_file level=2 number=5 size=4096 smallest={user_key='a' sequence=3 "
              "kind=deletion} largest={user_key='z' sequence=1 kind=value}\n");
}

TEST(DumpFileTest, PrintsMalformedManifestRecordContinuesAndReturnsCorruption) {
    MemoryFileSystem file_system;
    const std::filesystem::path path = "MANIFEST-000006";
    VersionEdit valid;
    valid.SetLogNumber(7);
    WriteRecords(file_system, path, {Bytes({0xff}), valid.Encode()});
    StringOutput output;

    const Status dumped = DumpFile(file_system, path, output);

    ASSERT_FALSE(dumped.has_value());
    EXPECT_EQ(dumped.error().code(), ErrorCode::Corruption);
    EXPECT_NE(output.text().find("record_error offset=0 error='corruption:"), std::string::npos);
    EXPECT_NE(output.text().find("record offset=8\n  log_number value=7\n"), std::string::npos);
}

TEST(DumpFileTest, PropagatesOutputFailureFromEveryRenderedSection) {
    MemoryFileSystem file_system;

    const std::filesystem::path log_path = "000007.log";
    EncodedWriteBatch batch;
    batch.Put(AsBytes("a"), AsBytes("1"));
    batch.Delete(AsBytes("b"));
    batch.SetSequence(1);
    WriteRecords(file_system, log_path, {Materialize(batch.encoded())});

    const std::filesystem::path corrupt_path = "000008.log";
    std::vector<std::byte> full_block_payload(WalBlockSize - WalHeaderSize, std::byte{'x'});
    WriteRecords(file_system, corrupt_path,
                 {full_block_payload, Batch(3, AsBytes("after"), AsBytes("ok"))});
    std::vector<std::byte> corrupted = *file_system.Contents(corrupt_path);
    corrupted[0] ^= std::byte{0x01};
    file_system.Write(corrupt_path, std::move(corrupted));

    const std::filesystem::path manifest_path = "MANIFEST-000009";
    VersionEdit edit;
    PopulateEveryManifestField(edit);
    WriteRecords(file_system, manifest_path, {edit.Encode()});

    const std::filesystem::path malformed_log_path = "000010.log";
    WriteRecords(file_system, malformed_log_path, {Bytes({0x01, 0x02})});
    const std::filesystem::path malformed_manifest_path = "MANIFEST-000010";
    WriteRecords(file_system, malformed_manifest_path, {Bytes({0xff})});

    const std::filesystem::path table_path = "000009.ldb";
    BuildTable(file_system, table_path, BytewiseComparator(), BlockCompression::None);

    for (const std::filesystem::path& path : {log_path, manifest_path, table_path}) {
        SCOPED_TRACE(path.string());
        StringOutput complete;
        static_cast<void>(DumpFile(file_system, path, complete));
        ASSERT_FALSE(complete.appends().empty());
        for (std::size_t fail_at = 0; fail_at < complete.appends().size(); ++fail_at) {
            SCOPED_TRACE(fail_at);
            StringOutput failed(fail_at);
            const Status status = DumpFile(file_system, path, failed);
            ASSERT_FALSE(status.has_value());
            EXPECT_EQ(status.error().message(), "injected output failure");
        }
    }

    StringOutput corrupt_failed(1);
    const Status corruption_output = DumpFile(file_system, corrupt_path, corrupt_failed);
    ASSERT_FALSE(corruption_output.has_value());
    EXPECT_EQ(corruption_output.error().message(), "injected output failure");

    for (const std::filesystem::path& path : {malformed_log_path, malformed_manifest_path}) {
        StringOutput malformed_failed(1);
        const Status malformed_output = DumpFile(file_system, path, malformed_failed);
        ASSERT_FALSE(malformed_output.has_value());
        EXPECT_EQ(malformed_output.error().message(), "injected output failure");
    }
}

TEST(DumpFileTest, DumpsTablesAcrossCompressionModesAndAlwaysShowsDeletionValues) {
    std::optional<std::string> expected_entries;
    std::uint64_t number = 10;
    for (const BlockCompression compression :
         {BlockCompression::None, BlockCompression::Snappy, BlockCompression::Zstd}) {
        SCOPED_TRACE(static_cast<int>(compression));
        MemoryFileSystem file_system;
        const std::filesystem::path path =
            std::filesystem::path("db") / (std::to_string(number++) + ".ldb");
        // Use canonical six-digit names.
        const std::filesystem::path canonical =
            std::filesystem::path("db") /
            (std::string(6 - path.filename().stem().string().size(), '0') +
             path.filename().stem().string() + ".ldb");
        BuildTable(file_system, canonical, BytewiseComparator(), compression);
        StringOutput output;

        const Status dumped = DumpFile(file_system, canonical, output);

        ASSERT_TRUE(dumped.has_value()) << dumped.error().ToString();
        const std::string text = output.text();
        const std::size_t entries_start = text.find('\n') + 1;
        const std::string entries = text.substr(entries_start);
        if (!expected_entries.has_value()) {
            expected_entries = entries;
        }
        EXPECT_EQ(entries, *expected_entries);
    }
    EXPECT_EQ(*expected_entries,
              "entry user_key='a' sequence=3 kind=value value='one'\n"
              "entry user_key='a' sequence=2 kind=deletion value='unexpected'\n"
              "entry user_key='b\\x00\\xff' sequence=1 kind=value value='v\\n'\n");
}

TEST(DumpFileTest, ForwardTableDumpDoesNotNeedTheDatabaseComparator) {
    MemoryFileSystem file_system;
    const std::filesystem::path path = "000020.ldb";
    const ReverseComparator comparator;
    BuildTable(file_system, path, comparator, BlockCompression::None);
    StringOutput output;

    const Status dumped = DumpFile(file_system, path, output);

    ASSERT_TRUE(dumped.has_value()) << dumped.error().ToString();
    const std::size_t b =
        output.text().find("entry user_key='b' sequence=3 kind=value value='two'\n");
    const std::size_t a =
        output.text().find("entry user_key='a' sequence=2 kind=deletion value='unexpected'\n");
    ASSERT_NE(b, std::string::npos);
    ASSERT_NE(a, std::string::npos);
    EXPECT_LT(b, a);
}

TEST(DumpFileTest, ReportsReachableBadInternalKeysButNotUnrequestedFilterDamage) {
    MemoryFileSystem file_system;
    const std::filesystem::path bad_key_path = "000021.ldb";
    const std::vector<std::byte> data = EncodeBlock({{AsBytes("bad"), AsBytes("value")}});
    const std::vector<std::byte> table =
        AssembleSingleDataBlockTable(data, Key(AsBytes("z"), MaxSequenceNumber).encoded());
    file_system.Write(bad_key_path, table);
    StringOutput bad_key_output;

    const Status bad_key = DumpFile(file_system, bad_key_path, bad_key_output);

    ASSERT_FALSE(bad_key.has_value());
    EXPECT_EQ(bad_key.error().code(), ErrorCode::Corruption);
    EXPECT_NE(bad_key_output.text().find("bad_key encoded='bad' value='value'\n"),
              std::string::npos);

    const std::filesystem::path filter_path = "000022.ldb";
    BuildTable(file_system, filter_path, BytewiseComparator(), BlockCompression::None,
               BloomFilterPolicy(10));
    std::vector<std::byte> filtered = *file_system.Contents(filter_path);
    const std::uint64_t filter_offset = FilterBlockOffset(filtered);
    ASSERT_LT(filter_offset, filtered.size());
    filtered[static_cast<std::size_t>(filter_offset)] ^= std::byte{0x01};
    file_system.Write(filter_path, std::move(filtered));
    StringOutput filter_output;

    const Status unrequested_filter = DumpFile(file_system, filter_path, filter_output);

    EXPECT_TRUE(unrequested_filter.has_value()) << unrequested_filter.error().ToString();
}

TEST(DumpFileTest, ReturnsReachableDataBlockChecksumFailures) {
    MemoryFileSystem file_system;
    const std::filesystem::path path = "000023.ldb";
    BuildTable(file_system, path, BytewiseComparator(), BlockCompression::None);
    std::vector<std::byte> corrupted = *file_system.Contents(path);
    corrupted[0] ^= std::byte{0x01};
    file_system.Write(path, std::move(corrupted));
    StringOutput output;

    const Status dumped = DumpFile(file_system, path, output);

    ASSERT_FALSE(dumped.has_value());
    EXPECT_EQ(dumped.error().code(), ErrorCode::Corruption);
    EXPECT_EQ(output.text(), "dump version=1 type=table file='000023.ldb'\n");
}

TEST(DumpFileTest, ReturnsTheFirstCorruptionWhenALaterBlockIsAlsoDamaged) {
    MemoryFileSystem file_system;
    const std::filesystem::path path = "000024.ldb";
    TwoBlockTable table = BadKeyThenValidTable();
    table.contents[static_cast<std::size_t>(table.second_offset)] ^= std::byte{0x01};
    file_system.Write(path, std::move(table.contents));
    StringOutput output;

    const Status dumped = DumpFile(file_system, path, output);

    ASSERT_FALSE(dumped.has_value());
    EXPECT_EQ(dumped.error().code(), ErrorCode::Corruption);
    EXPECT_EQ(dumped.error().message(), "table entry has a malformed internal key");
    EXPECT_NE(output.text().find("bad_key encoded='bad' value='first'\n"), std::string::npos);
}

TEST(DumpFileTest, LaterIoFailureTakesPrecedenceOverRememberedCorruption) {
    MemoryFileSystem file_system;
    const std::filesystem::path path = "000025.ldb";
    TwoBlockTable table = BadKeyThenValidTable();
    file_system.Write(path, std::move(table.contents));
    // Size, open, footer, index, and first data reads succeed; the second data read fails.
    file_system.FailOperation(file_system.operations().size() + 5, Error::Io("second read failed"));
    StringOutput output;

    const Status dumped = DumpFile(file_system, path, output);

    ASSERT_FALSE(dumped.has_value());
    EXPECT_EQ(dumped.error().code(), ErrorCode::Io);
    EXPECT_EQ(dumped.error().message(), "second read failed");
    EXPECT_NE(output.text().find("bad_key encoded='bad' value='first'\n"), std::string::npos);
}

TEST(DumpFileTest, PropagatesTableSizeOpenAndFooterFailuresBeforeOutput) {
    MemoryFileSystem file_system;
    StringOutput missing_output;
    const Status missing = DumpFile(file_system, "000026.ldb", missing_output);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
    EXPECT_TRUE(missing_output.text().empty());

    file_system.Write("000026.ldb", {});
    file_system.FailOperation(file_system.operations().size() + 1, Error::Io("open failed"));
    StringOutput open_output;
    const Status open_failed = DumpFile(file_system, "000026.ldb", open_output);
    ASSERT_FALSE(open_failed.has_value());
    EXPECT_EQ(open_failed.error().message(), "open failed");
    EXPECT_TRUE(open_output.text().empty());

    StringOutput footer_output;
    const Status footer_failed = DumpFile(file_system, "000026.ldb", footer_output);
    ASSERT_FALSE(footer_failed.has_value());
    EXPECT_EQ(footer_failed.error().code(), ErrorCode::Corruption);
    EXPECT_TRUE(footer_output.text().empty());
}

TEST(DumpFileTest, PropagatesSequentialReadFailures) {
    MemoryFileSystem file_system;
    const std::filesystem::path path = "000027.log";
    WriteRecords(file_system, path, {Batch(1, AsBytes("a"), AsBytes("1"))});
    // Open succeeds, then the first read fails.
    file_system.FailOperation(file_system.operations().size() + 1, Error::Io("read failed"));
    StringOutput output;

    const Status dumped = DumpFile(file_system, path, output);

    ASSERT_FALSE(dumped.has_value());
    EXPECT_EQ(dumped.error().message(), "read failed");
    EXPECT_EQ(output.text(), "dump version=1 type=log file='000027.log'\n");
}

TEST(DumpFileTest, RejectsUnsupportedNamesAndPropagatesOutputAndInputFailures) {
    MemoryFileSystem file_system;
    for (const std::string_view name : {"CURRENT", "LOCK", "000001.dbtmp", "1.log", "unknown"}) {
        SCOPED_TRACE(name);
        file_system.Write(std::string(name), {});
        StringOutput unsupported_output;
        const Status unsupported = DumpFile(file_system, std::string(name), unsupported_output);
        ASSERT_FALSE(unsupported.has_value());
        EXPECT_EQ(unsupported.error().code(), ErrorCode::InvalidArgument);
        EXPECT_TRUE(unsupported_output.text().empty());
    }

    const std::filesystem::path path = "000030.log";
    WriteRecords(file_system, path, {Batch(1, AsBytes("a"), AsBytes("1"))});
    StringOutput failed_output(1);
    const Status output_failed = DumpFile(file_system, path, failed_output);
    ASSERT_FALSE(output_failed.has_value());
    EXPECT_EQ(output_failed.error().message(), "injected output failure");

    file_system.FailOperation(file_system.operations().size(), Error::Io("injected input failure"));
    StringOutput input_output;
    const Status input_failed = DumpFile(file_system, path, input_output);
    ASSERT_FALSE(input_failed.has_value());
    EXPECT_EQ(input_failed.error().message(), "injected input failure");
}

}  // namespace
}  // namespace modern_leveldb
