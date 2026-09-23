#include "wal/wal_io.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "format/wal_format.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/crc32c.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"

namespace modern_leveldb {
namespace {

static_assert(!std::is_copy_constructible_v<WalWriter>);
static_assert(!std::is_copy_assignable_v<WalWriter>);
static_assert(!std::is_move_constructible_v<WalWriter>);
static_assert(!std::is_move_assignable_v<WalWriter>);
static_assert(!std::is_copy_constructible_v<WalReader>);
static_assert(!std::is_copy_assignable_v<WalReader>);
static_assert(!std::is_move_constructible_v<WalReader>);
static_assert(!std::is_move_assignable_v<WalReader>);

struct WritableState {
  std::vector<std::byte> data;
  std::vector<std::size_t> append_sizes;
  int append_calls = 0;
  int flush_calls = 0;
  int sync_calls = 0;
  int close_calls = 0;
  std::optional<int> fail_append_call;
  bool fail_flush = false;
  bool fail_sync = false;
  bool fail_close = false;
};

class TrackingWritableFile final : public WritableFile {
 public:
  explicit TrackingWritableFile(std::shared_ptr<WritableState> state)
      : state_(std::move(state)) {}

  Status Append(ByteView data) override {
    ++state_->append_calls;
    state_->append_sizes.push_back(data.size());
    if (state_->fail_append_call == state_->append_calls) {
      return std::unexpected(Error::Io("injected append failure"));
    }
    state_->data.insert(state_->data.end(), data.begin(), data.end());
    return {};
  }

  Status Flush() override {
    ++state_->flush_calls;
    if (state_->fail_flush) {
      return std::unexpected(Error::Io("injected flush failure"));
    }
    return {};
  }

  Status Sync() override {
    ++state_->sync_calls;
    if (state_->fail_sync) {
      return std::unexpected(Error::Io("injected sync failure"));
    }
    return {};
  }

  Status Close() override {
    ++state_->close_calls;
    if (state_->fail_close) {
      return std::unexpected(Error::Io("injected close failure"));
    }
    return {};
  }

 private:
  std::shared_ptr<WritableState> state_;
};

struct SequentialState {
  std::vector<std::byte> data;
  std::size_t position = 0;
  std::size_t maximum_chunk = std::numeric_limits<std::size_t>::max();
  int read_calls = 0;
  int skip_calls = 0;
  std::optional<int> fail_read_call;
  bool fail_skip = false;
  bool return_oversized_count = false;
};

class TrackingSequentialFile final : public SequentialFile {
 public:
  explicit TrackingSequentialFile(std::shared_ptr<SequentialState> state)
      : state_(std::move(state)) {}

  Result<std::size_t> Read(MutableByteView output) override {
    ++state_->read_calls;
    if (state_->fail_read_call == state_->read_calls) {
      return std::unexpected(Error::Io("injected read failure"));
    }
    if (state_->return_oversized_count) {
      return output.size() + 1U;
    }
    if (output.empty() || state_->position == state_->data.size()) {
      return 0U;
    }

    const std::size_t size =
        std::min({output.size(), state_->maximum_chunk,
                  state_->data.size() - state_->position});
    std::ranges::copy(
        ByteView(state_->data).subspan(state_->position, size), output.begin());
    state_->position += size;
    return size;
  }

  Status Skip(std::uint64_t bytes) override {
    ++state_->skip_calls;
    if (state_->fail_skip) {
      return std::unexpected(Error::Io("injected skip failure"));
    }
    const std::uint64_t remaining = state_->data.size() - state_->position;
    if (bytes >= remaining) {
      state_->position = state_->data.size();
    } else {
      state_->position += static_cast<std::size_t>(bytes);
    }
    return {};
  }

 private:
  std::shared_ptr<SequentialState> state_;
};

std::vector<std::byte> Bytes(std::initializer_list<unsigned int> values) {
  std::vector<std::byte> result;
  result.reserve(values.size());
  for (const unsigned int value : values) {
    result.push_back(static_cast<std::byte>(value));
  }
  return result;
}

std::vector<std::byte> Pattern(std::size_t size) {
  std::vector<std::byte> result(size);
  for (std::size_t index = 0; index < size; ++index) {
    result[index] = static_cast<std::byte>((index * 29U + 7U) & 0xffU);
  }
  return result;
}

std::vector<std::byte> PhysicalRecord(std::uint8_t type, ByteView payload,
                                      bool valid_checksum = true) {
  std::vector<std::byte> result;
  const std::byte type_byte = static_cast<std::byte>(type);
  std::uint32_t crc = ExtendCrc32c(Crc32c(ByteView(&type_byte, 1)), payload);
  crc = MaskCrc32c(crc);
  if (!valid_checksum) {
    ++crc;
  }
  AppendFixed32(result, crc);
  result.push_back(static_cast<std::byte>(payload.size() & 0xffU));
  result.push_back(static_cast<std::byte>((payload.size() >> 8U) & 0xffU));
  result.push_back(type_byte);
  result.insert(result.end(), payload.begin(), payload.end());
  return result;
}

std::unique_ptr<WritableFile> Writable(const std::shared_ptr<WritableState>& state) {
  return std::make_unique<TrackingWritableFile>(state);
}

std::unique_ptr<SequentialFile> Sequential(
    const std::shared_ptr<SequentialState>& state) {
  return std::make_unique<TrackingSequentialFile>(state);
}

struct CopiedRecord {
  std::vector<std::byte> data;
  std::uint64_t offset;
};

std::optional<CopiedRecord> NextRecord(WalReader& reader) {
  WalReadResult result = reader.ReadNext();
  if (!result.has_value()) {
    ADD_FAILURE() << result.error().ToString();
    return std::nullopt;
  }
  if (!result->has_value()) {
    ADD_FAILURE() << "expected WAL record, got EOF";
    return std::nullopt;
  }
  const auto* record = std::get_if<WalLogicalRecord>(&**result);
  if (record == nullptr) {
    ADD_FAILURE() << "expected WAL record, got corruption";
    return std::nullopt;
  }
  return CopiedRecord{
      .data = {record->data.begin(), record->data.end()},
      .offset = record->offset,
  };
}

std::optional<WalCorruption> NextCorruption(WalReader& reader) {
  WalReadResult result = reader.ReadNext();
  if (!result.has_value()) {
    ADD_FAILURE() << result.error().ToString();
    return std::nullopt;
  }
  if (!result->has_value()) {
    ADD_FAILURE() << "expected WAL corruption, got EOF";
    return std::nullopt;
  }
  const auto* corruption = std::get_if<WalCorruption>(&**result);
  if (corruption == nullptr) {
    ADD_FAILURE() << "expected WAL corruption, got record";
    return std::nullopt;
  }
  return *corruption;
}

void ExpectEof(WalReader& reader) {
  const WalReadResult result = reader.ReadNext();
  ASSERT_TRUE(result.has_value()) << result.error().ToString();
  EXPECT_FALSE(result->has_value());
}

TEST(WalWriterTest, WritesExactRecordsFlushesSyncsAndClosesExplicitly) {
  const auto state = std::make_shared<WritableState>();
  WalWriter writer(Writable(state));

  ASSERT_TRUE(writer.AddRecord(AsBytes("foo")).has_value());
  ASSERT_TRUE(writer.AddRecord({}).has_value());

  EXPECT_EQ(state->data,
            Bytes({
                0xdd, 0x5f, 0xb3, 0x7a, 0x03, 0x00, 0x01, 0x66, 0x6f, 0x6f,
                0x05, 0x2b, 0x28, 0x43, 0x00, 0x00, 0x01,
            }));
  EXPECT_EQ(state->flush_calls, 2);
  EXPECT_EQ(state->sync_calls, 0);
  EXPECT_EQ(state->close_calls, 0);

  ASSERT_TRUE(writer.Sync().has_value());
  ASSERT_TRUE(writer.Close().has_value());
  EXPECT_EQ(state->sync_calls, 1);
  EXPECT_EQ(state->close_calls, 1);

  EXPECT_EQ(writer.AddRecord(AsBytes("late")).error().code(),
            ErrorCode::InvalidArgument);
  EXPECT_EQ(writer.Sync().error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(writer.Close().error().code(), ErrorCode::InvalidArgument);
}

TEST(WalWriterTest, ReopenPadsPartialBlockBeforeFirstNewRecord) {
  const auto state = std::make_shared<WritableState>();
  state->data = PhysicalRecord(
      static_cast<std::uint8_t>(WalRecordType::Full), AsBytes("old"));
  state->data.pop_back();
  const std::size_t truncated_size = state->data.size();
  WalWriter writer(Writable(state), truncated_size);

  ASSERT_TRUE(writer.AddRecord(AsBytes("new")).has_value());

  ASSERT_EQ(state->data.size(), WalBlockSize + WalHeaderSize + 3U);
  EXPECT_TRUE(std::ranges::all_of(
      ByteView(state->data).subspan(truncated_size, WalBlockSize - truncated_size),
      [](std::byte value) { return value == std::byte{0}; }));
  EXPECT_EQ(
      std::vector<std::byte>(state->data.begin() + static_cast<std::ptrdiff_t>(WalBlockSize),
                             state->data.end()),
      PhysicalRecord(static_cast<std::uint8_t>(WalRecordType::Full), AsBytes("new")));

  const auto source = std::make_shared<SequentialState>();
  source->data = state->data;
  WalReader reader(Sequential(source));
  const auto corruption = NextCorruption(reader);
  ASSERT_TRUE(corruption.has_value());
  EXPECT_EQ(corruption->error.code(), ErrorCode::Corruption);
  const auto record = NextRecord(reader);
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(AsStringView(record->data), "new");
  EXPECT_EQ(record->offset, WalBlockSize);
}

TEST(WalWriterTest, AlignedReopenDoesNotAddAnExtraBlock) {
  const auto state = std::make_shared<WritableState>();
  state->data.resize(WalBlockSize, std::byte{0});
  WalWriter writer(Writable(state), state->data.size());

  ASSERT_TRUE(writer.AddRecord(AsBytes("x")).has_value());

  EXPECT_EQ(state->data.size(), WalBlockSize + WalHeaderSize + 1U);
  EXPECT_EQ(state->data[WalBlockSize + 6U],
            static_cast<std::byte>(WalRecordType::Full));
}

TEST(WalWriterTest, FirstIoErrorPoisonsWriterButCloseStillRuns) {
  const auto state = std::make_shared<WritableState>();
  state->fail_append_call = 2;
  state->fail_close = true;
  WalWriter writer(Writable(state));

  const Status first = writer.AddRecord(AsBytes("payload"));
  ASSERT_FALSE(first.has_value());
  EXPECT_EQ(first.error().message(), "injected append failure");
  const int append_calls = state->append_calls;

  const Status second = writer.AddRecord(AsBytes("ignored"));
  const Status sync = writer.Sync();
  const Status close = writer.Close();

  ASSERT_FALSE(second.has_value());
  ASSERT_FALSE(sync.has_value());
  ASSERT_FALSE(close.has_value());
  EXPECT_EQ(second.error().message(), first.error().message());
  EXPECT_EQ(sync.error().message(), first.error().message());
  EXPECT_EQ(close.error().message(), first.error().message());
  EXPECT_EQ(state->append_calls, append_calls);
  EXPECT_EQ(state->sync_calls, 0);
  EXPECT_EQ(state->close_calls, 1);
}

TEST(WalWriterTest, FlushAndSyncFailuresPoisonWriter) {
  const auto flush_state = std::make_shared<WritableState>();
  flush_state->fail_flush = true;
  WalWriter flush_writer(Writable(flush_state));
  const Status flush = flush_writer.AddRecord(AsBytes("record"));
  ASSERT_FALSE(flush.has_value());
  EXPECT_EQ(flush.error().message(), "injected flush failure");
  EXPECT_EQ(flush_state->flush_calls, 1);
  EXPECT_EQ(flush_writer.Sync().error().message(), flush.error().message());
  EXPECT_EQ(flush_state->sync_calls, 0);

  const auto sync_state = std::make_shared<WritableState>();
  sync_state->fail_sync = true;
  WalWriter sync_writer(Writable(sync_state));
  ASSERT_TRUE(sync_writer.AddRecord(AsBytes("record")).has_value());
  const Status sync = sync_writer.Sync();
  ASSERT_FALSE(sync.has_value());
  EXPECT_EQ(sync.error().message(), "injected sync failure");
  EXPECT_EQ(sync_writer.AddRecord(AsBytes("ignored")).error().message(),
            sync.error().message());
}

TEST(WalWriterTest, NullFileReturnsInvalidArgumentWithoutDereference) {
  WalWriter writer(nullptr);

  EXPECT_EQ(writer.AddRecord(AsBytes("record")).error().code(),
            ErrorCode::InvalidArgument);
  EXPECT_EQ(writer.Sync().error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(writer.Close().error().code(), ErrorCode::InvalidArgument);
}

TEST(WalReaderTest, RoundTripsShortReadsEmptyAndFragmentedRecords) {
  const auto destination = std::make_shared<WritableState>();
  WalWriter writer(Writable(destination));
  const std::vector<std::byte> medium = Pattern(50'000);
  const std::vector<std::byte> large = Pattern(100'000);
  ASSERT_TRUE(writer.AddRecord(AsBytes("foo")).has_value());
  ASSERT_TRUE(writer.AddRecord({}).has_value());
  ASSERT_TRUE(writer.AddRecord(medium).has_value());
  ASSERT_TRUE(writer.AddRecord(large).has_value());

  const auto source = std::make_shared<SequentialState>();
  source->data = destination->data;
  source->maximum_chunk = 13;
  WalReader reader(Sequential(source));

  const auto first = NextRecord(reader);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(AsStringView(first->data), "foo");
  EXPECT_EQ(first->offset, 0U);
  const auto empty = NextRecord(reader);
  ASSERT_TRUE(empty.has_value());
  EXPECT_TRUE(empty->data.empty());
  EXPECT_EQ(empty->offset, WalHeaderSize + 3U);
  const auto medium_record = NextRecord(reader);
  ASSERT_TRUE(medium_record.has_value());
  EXPECT_EQ(medium_record->data, medium);
  const auto large_record = NextRecord(reader);
  ASSERT_TRUE(large_record.has_value());
  EXPECT_EQ(large_record->data, large);
  ExpectEof(reader);
  ExpectEof(reader);
  EXPECT_GT(source->read_calls, 4);
}

TEST(WalReaderTest, UnknownTypeEmitsCorruptionThenContinues) {
  const std::vector<std::byte> unknown = PhysicalRecord(9, AsBytes("unknown"));
  std::vector<std::byte> encoded = unknown;
  const std::vector<std::byte> good =
      PhysicalRecord(static_cast<std::uint8_t>(WalRecordType::Full),
                     AsBytes("good"));
  encoded.insert(encoded.end(), good.begin(), good.end());
  const auto source = std::make_shared<SequentialState>();
  source->data = encoded;
  WalReader reader(Sequential(source));

  const auto corruption = NextCorruption(reader);
  ASSERT_TRUE(corruption.has_value());
  EXPECT_EQ(corruption->offset, 0U);
  EXPECT_EQ(corruption->dropped_bytes, unknown.size());
  const auto record = NextRecord(reader);
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(AsStringView(record->data), "good");
  EXPECT_EQ(record->offset, unknown.size());
  ExpectEof(reader);
}

TEST(WalReaderTest, ChecksumFailureDropsBlockButNotNextBlock) {
  std::vector<std::byte> encoded =
      PhysicalRecord(static_cast<std::uint8_t>(WalRecordType::Full),
                     AsBytes("bad"), false);
  encoded.resize(WalBlockSize, std::byte{0x7f});
  const std::vector<std::byte> good =
      PhysicalRecord(static_cast<std::uint8_t>(WalRecordType::Full),
                     AsBytes("good"));
  encoded.insert(encoded.end(), good.begin(), good.end());
  const auto source = std::make_shared<SequentialState>();
  source->data = encoded;
  WalReader reader(Sequential(source));

  const auto corruption = NextCorruption(reader);
  ASSERT_TRUE(corruption.has_value());
  EXPECT_EQ(corruption->offset, 0U);
  EXPECT_EQ(corruption->dropped_bytes, WalBlockSize);
  const auto record = NextRecord(reader);
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(AsStringView(record->data), "good");
  EXPECT_EQ(record->offset, WalBlockSize);
}

TEST(WalReaderTest, PhysicalCorruptionNeverJoinsFragmentedRecords) {
  std::vector<std::byte> encoded =
      PhysicalRecord(static_cast<std::uint8_t>(WalRecordType::First),
                     AsBytes("first-part"));
  const std::vector<std::byte> corrupt =
      PhysicalRecord(static_cast<std::uint8_t>(WalRecordType::Middle),
                     AsBytes("corrupt"), false);
  encoded.insert(encoded.end(), corrupt.begin(), corrupt.end());
  encoded.resize(WalBlockSize, std::byte{0x7f});
  const std::vector<std::byte> orphan_last =
      PhysicalRecord(static_cast<std::uint8_t>(WalRecordType::Last),
                     AsBytes("last-part"));
  const std::vector<std::byte> good =
      PhysicalRecord(static_cast<std::uint8_t>(WalRecordType::Full),
                     AsBytes("good"));
  encoded.insert(encoded.end(), orphan_last.begin(), orphan_last.end());
  encoded.insert(encoded.end(), good.begin(), good.end());
  const auto source = std::make_shared<SequentialState>();
  source->data = encoded;
  WalReader reader(Sequential(source));

  const auto physical = NextCorruption(reader);
  ASSERT_TRUE(physical.has_value());
  EXPECT_EQ(physical->offset, 0U);
  const auto orphan = NextCorruption(reader);
  ASSERT_TRUE(orphan.has_value());
  EXPECT_EQ(orphan->offset, WalBlockSize);
  const auto record = NextRecord(reader);
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(AsStringView(record->data), "good");
}

TEST(WalReaderTest, TruncatedTailAndIncompleteFragmentAreBenignEof) {
  std::vector<std::byte> truncated_header =
      PhysicalRecord(static_cast<std::uint8_t>(WalRecordType::Full),
                     AsBytes("good"));
  truncated_header.insert(
      truncated_header.end(),
      {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}});
  auto header_source = std::make_shared<SequentialState>();
  header_source->data = truncated_header;
  WalReader header_reader(Sequential(header_source));
  EXPECT_EQ(AsStringView(NextRecord(header_reader)->data), "good");
  ExpectEof(header_reader);

  std::vector<std::byte> truncated_payload =
      PhysicalRecord(static_cast<std::uint8_t>(WalRecordType::Full),
                     AsBytes("tail"));
  truncated_payload.pop_back();
  auto payload_source = std::make_shared<SequentialState>();
  payload_source->data = truncated_payload;
  WalReader payload_reader(Sequential(payload_source));
  ExpectEof(payload_reader);

  auto fragment_source = std::make_shared<SequentialState>();
  fragment_source->data =
      PhysicalRecord(static_cast<std::uint8_t>(WalRecordType::First),
                     AsBytes("partial"));
  WalReader fragment_reader(Sequential(fragment_source));
  ExpectEof(fragment_reader);

  std::vector<std::byte> oversized_length(WalHeaderSize, std::byte{0});
  oversized_length[4] = std::byte{0xff};
  oversized_length[5] = std::byte{0xff};
  oversized_length[6] = static_cast<std::byte>(WalRecordType::Full);
  auto oversized_source = std::make_shared<SequentialState>();
  oversized_source->data = oversized_length;
  WalReader oversized_reader(Sequential(oversized_source));
  ExpectEof(oversized_reader);
}

TEST(WalReaderTest, UnexpectedAndInterruptedFragmentsEmitSeparateEvents) {
  std::vector<std::byte> encoded =
      PhysicalRecord(static_cast<std::uint8_t>(WalRecordType::Middle),
                     AsBytes("orphan"));
  const std::vector<std::byte> first =
      PhysicalRecord(static_cast<std::uint8_t>(WalRecordType::First),
                     AsBytes("partial"));
  const std::vector<std::byte> full =
      PhysicalRecord(static_cast<std::uint8_t>(WalRecordType::Full),
                     AsBytes("good"));
  encoded.insert(encoded.end(), first.begin(), first.end());
  encoded.insert(encoded.end(), full.begin(), full.end());
  const auto source = std::make_shared<SequentialState>();
  source->data = encoded;
  WalReader reader(Sequential(source));

  const auto orphan = NextCorruption(reader);
  ASSERT_TRUE(orphan.has_value());
  EXPECT_EQ(orphan->dropped_bytes, 6U);
  const auto interrupted = NextCorruption(reader);
  ASSERT_TRUE(interrupted.has_value());
  EXPECT_EQ(interrupted->dropped_bytes, 7U);
  const auto record = NextRecord(reader);
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(AsStringView(record->data), "good");
}

TEST(WalReaderTest, EmptyFirstCompatibilityAndZeroMarkerStateReset) {
  std::vector<std::byte> compatible =
      PhysicalRecord(static_cast<std::uint8_t>(WalRecordType::First), {});
  const std::vector<std::byte> full =
      PhysicalRecord(static_cast<std::uint8_t>(WalRecordType::Full),
                     AsBytes("compatible"));
  compatible.insert(compatible.end(), full.begin(), full.end());
  auto compatible_source = std::make_shared<SequentialState>();
  compatible_source->data = compatible;
  WalReader compatible_reader(Sequential(compatible_source));
  EXPECT_EQ(AsStringView(NextRecord(compatible_reader)->data), "compatible");
  ExpectEof(compatible_reader);

  std::vector<std::byte> zeroed =
      PhysicalRecord(static_cast<std::uint8_t>(WalRecordType::First),
                     AsBytes("partial"));
  zeroed.insert(zeroed.end(), WalHeaderSize, std::byte{0});
  zeroed.resize(WalBlockSize, std::byte{0});
  const std::vector<std::byte> last =
      PhysicalRecord(static_cast<std::uint8_t>(WalRecordType::Last),
                     AsBytes("orphan-last"));
  const std::vector<std::byte> good =
      PhysicalRecord(static_cast<std::uint8_t>(WalRecordType::Full),
                     AsBytes("good"));
  zeroed.insert(zeroed.end(), last.begin(), last.end());
  zeroed.insert(zeroed.end(), good.begin(), good.end());
  auto zero_source = std::make_shared<SequentialState>();
  zero_source->data = zeroed;
  WalReader zero_reader(Sequential(zero_source));

  EXPECT_EQ(NextCorruption(zero_reader)->dropped_bytes, 7U);
  EXPECT_EQ(NextCorruption(zero_reader)->dropped_bytes, 11U);
  EXPECT_EQ(AsStringView(NextRecord(zero_reader)->data), "good");
}

TEST(WalReaderTest, InitialOffsetsSkipInProgressRecordsAndReportRecordOffsets) {
  const auto destination = std::make_shared<WritableState>();
  WalWriter writer(Writable(destination));
  const std::vector<std::byte> first(10'000, std::byte{'a'});
  const std::vector<std::byte> second(10'000, std::byte{'b'});
  const std::vector<std::byte> spanning(2U * WalBlockSize - 1'000U,
                                        std::byte{'c'});
  const std::vector<std::byte> near_block_end(13'716, std::byte{'e'});
  const std::vector<std::byte> full_block_record(WalBlockSize - WalHeaderSize,
                                                 std::byte{'f'});
  ASSERT_TRUE(writer.AddRecord(first).has_value());
  ASSERT_TRUE(writer.AddRecord(second).has_value());
  ASSERT_TRUE(writer.AddRecord(spanning).has_value());
  ASSERT_TRUE(writer.AddRecord(AsBytes("d")).has_value());
  ASSERT_TRUE(writer.AddRecord(near_block_end).has_value());
  ASSERT_TRUE(writer.AddRecord(full_block_record).has_value());

  constexpr std::uint64_t SecondOffset = WalHeaderSize + 10'000U;
  constexpr std::uint64_t ThirdOffset = 2U * (WalHeaderSize + 10'000U);
  constexpr std::uint64_t FourthOffset =
      ThirdOffset + (2U * WalBlockSize - 1'000U) + 3U * WalHeaderSize;

  auto read_from = [&](std::uint64_t offset) {
    const auto source = std::make_shared<SequentialState>();
    source->data = destination->data;
    WalReader reader(Sequential(source), {.initial_offset = offset});
    return NextRecord(reader);
  };

  const auto at_start = read_from(0);
  ASSERT_TRUE(at_start.has_value());
  EXPECT_EQ(at_start->data, first);
  EXPECT_EQ(at_start->offset, 0U);

  const auto inside_first = read_from(1);
  ASSERT_TRUE(inside_first.has_value());
  EXPECT_EQ(inside_first->data, second);
  EXPECT_EQ(inside_first->offset, SecondOffset);

  const auto exact_second = read_from(SecondOffset);
  ASSERT_TRUE(exact_second.has_value());
  EXPECT_EQ(exact_second->data, second);
  EXPECT_EQ(exact_second->offset, SecondOffset);

  const auto inside_spanning = read_from(WalBlockSize + 1U);
  ASSERT_TRUE(inside_spanning.has_value());
  EXPECT_EQ(AsStringView(inside_spanning->data), "d");
  EXPECT_EQ(inside_spanning->offset, FourthOffset);

  const auto in_block_trailer = read_from(3U * WalBlockSize - 3U);
  ASSERT_TRUE(in_block_trailer.has_value());
  EXPECT_EQ(in_block_trailer->data, full_block_record);
  EXPECT_EQ(in_block_trailer->offset, 3U * WalBlockSize);

  const auto past_end_source = std::make_shared<SequentialState>();
  past_end_source->data = destination->data;
  WalReader past_end(
      Sequential(past_end_source),
      {.initial_offset = static_cast<std::uint64_t>(destination->data.size()) + 5U});
  ExpectEof(past_end);
}

TEST(WalReaderTest, SuppressesCorruptionBeforeInitialOffset) {
  std::vector<std::byte> encoded =
      PhysicalRecord(static_cast<std::uint8_t>(WalRecordType::Full),
                     AsBytes("bad"), false);
  encoded.resize(WalBlockSize, std::byte{0x7f});
  const std::vector<std::byte> good =
      PhysicalRecord(static_cast<std::uint8_t>(WalRecordType::Full),
                     AsBytes("good"));
  encoded.insert(encoded.end(), good.begin(), good.end());
  const auto source = std::make_shared<SequentialState>();
  source->data = encoded;
  WalReader reader(Sequential(source), {.initial_offset = 1});

  const auto record = NextRecord(reader);

  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(AsStringView(record->data), "good");
  EXPECT_EQ(record->offset, WalBlockSize);
  ExpectEof(reader);
}

TEST(WalReaderTest, ReadAndSkipErrorsAreTerminal) {
  const auto read_state = std::make_shared<SequentialState>();
  read_state->fail_read_call = 1;
  WalReader read_error(Sequential(read_state));

  const WalReadResult first_read = read_error.ReadNext();
  ASSERT_FALSE(first_read.has_value());
  EXPECT_EQ(first_read.error().message(), "injected read failure");
  const int read_calls = read_state->read_calls;
  const WalReadResult second_read = read_error.ReadNext();
  ASSERT_FALSE(second_read.has_value());
  EXPECT_EQ(second_read.error().message(), first_read.error().message());
  EXPECT_EQ(read_state->read_calls, read_calls);

  const auto skip_state = std::make_shared<SequentialState>();
  skip_state->fail_skip = true;
  WalReader skip_error(
      Sequential(skip_state), {.initial_offset = WalBlockSize});
  const WalReadResult first_skip = skip_error.ReadNext();
  ASSERT_FALSE(first_skip.has_value());
  EXPECT_EQ(first_skip.error().message(), "injected skip failure");
  const WalReadResult second_skip = skip_error.ReadNext();
  ASSERT_FALSE(second_skip.has_value());
  EXPECT_EQ(second_skip.error().message(), first_skip.error().message());
  EXPECT_EQ(skip_state->skip_calls, 1);

  const auto overflow_state = std::make_shared<SequentialState>();
  WalReader overflow(
      Sequential(overflow_state),
      {.initial_offset = std::numeric_limits<std::uint64_t>::max()});
  const WalReadResult overflow_result = overflow.ReadNext();
  ASSERT_FALSE(overflow_result.has_value());
  EXPECT_EQ(overflow_result.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(overflow_state->skip_calls, 0);
}

TEST(WalReaderTest, InvalidFileAndReadCountViolationsAreTerminal) {
  WalReader null_reader(nullptr);
  const WalReadResult null_result = null_reader.ReadNext();
  ASSERT_FALSE(null_result.has_value());
  EXPECT_EQ(null_result.error().code(), ErrorCode::InvalidArgument);

  const auto oversized_state = std::make_shared<SequentialState>();
  oversized_state->return_oversized_count = true;
  WalReader oversized_reader(Sequential(oversized_state));
  const WalReadResult first = oversized_reader.ReadNext();
  ASSERT_FALSE(first.has_value());
  EXPECT_EQ(first.error().code(), ErrorCode::Io);
  const int read_calls = oversized_state->read_calls;
  const WalReadResult second = oversized_reader.ReadNext();
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().message(), first.error().message());
  EXPECT_EQ(oversized_state->read_calls, read_calls);
}

}  // namespace
}  // namespace modern_leveldb
