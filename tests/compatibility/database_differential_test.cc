#include <gtest/gtest.h>
#include <leveldb/db.h>
#include <leveldb/env.h>
#include <leveldb/table_builder.h>
#include <leveldb/write_batch.h>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "support/database_model.h"
#include "support/temporary_directory.h"

namespace modern_leveldb {
namespace {

void CheckReference(const leveldb::Status& status) {
  if (!status.ok()) {
    throw std::runtime_error("reference: " + status.ToString());
  }
}

class ReferenceClient final {
 public:
  ReferenceClient(std::filesystem::path path, Compression compression) : path_(std::move(path)) {
    options_.create_if_missing = true;
    options_.write_buffer_size = 64 * 1024;
    options_.compression = static_cast<leveldb::CompressionType>(compression);
    Open();
  }
  ReferenceClient(const ReferenceClient&) = delete;
  ReferenceClient& operator=(const ReferenceClient&) = delete;
  ReferenceClient(ReferenceClient&&) = delete;
  ReferenceClient& operator=(ReferenceClient&&) = delete;
  void Close() {
    for (auto& snapshot : snapshots_) {
      snapshot.reset();
    }
    database_.reset();
  }
  void Reopen() {
    Close();
    Open();
  }
  void Put(std::string_view key, std::string_view value) {
    CheckReference(database_->Put({}, Slice(key), Slice(value)));
  }
  void Delete(std::string_view key) { CheckReference(database_->Delete({}, Slice(key))); }
  void Write(const test_support::ModelBatch& operations, bool sync) {
    leveldb::WriteBatch batch;
    for (const auto& [key, value] : operations) {
      if (value.has_value()) {
        batch.Put(Slice(key), Slice(*value));
      } else {
        batch.Delete(Slice(key));
      }
    }
    leveldb::WriteOptions options;
    options.sync = sync;
    CheckReference(database_->Write(options, &batch));
  }
  void SnapshotAt(std::size_t slot) {
    snapshots_[slot] = Snapshot(database_->GetSnapshot(), [this](const auto* snapshot) {
      database_->ReleaseSnapshot(snapshot);
    });
  }
  void Release(std::size_t slot) { snapshots_[slot].reset(); }
  std::optional<std::string> Read(std::string_view key,
                                  std::size_t slot = test_support::SnapshotSlots) {
    std::string value;
    const auto status = database_->Get(ReadAt(slot), Slice(key), &value);
    if (status.IsNotFound()) {
      return std::nullopt;
    }
    CheckReference(status);
    return value;
  }
  test_support::Entries Scan(bool reverse, std::size_t slot = test_support::SnapshotSlots,
                             std::optional<std::string_view> target = {}) {
    std::unique_ptr<leveldb::Iterator> iterator(database_->NewIterator(ReadAt(slot)));
    if (target.has_value()) {
      iterator->Seek(Slice(*target));
    } else if (reverse) {
      iterator->SeekToLast();
    } else {
      iterator->SeekToFirst();
    }
    test_support::Entries result;
    while (iterator->Valid()) {
      if (result.size() > 4096) {
        throw std::runtime_error("reference iterator did not terminate");
      }
      result.emplace_back(iterator->key().ToString(), iterator->value().ToString());
      if (reverse) {
        iterator->Prev();
      } else {
        iterator->Next();
      }
    }
    CheckReference(iterator->status());
    return result;
  }

 private:
  using Snapshot =
      std::unique_ptr<const leveldb::Snapshot, std::function<void(const leveldb::Snapshot*)>>;

  static leveldb::Slice Slice(std::string_view bytes) { return {bytes.data(), bytes.size()}; }
  void Open() {
    leveldb::DB* opened = nullptr;
    const auto status = leveldb::DB::Open(options_, path_.string(), &opened);
    database_.reset(opened);
    CheckReference(status);
  }
  leveldb::ReadOptions ReadAt(std::size_t slot) const {
    leveldb::ReadOptions options;
    options.verify_checksums = true;
    if (slot != test_support::SnapshotSlots) {
      options.snapshot = snapshots_.at(slot).get();
    }
    return options;
  }

  std::filesystem::path path_;
  leveldb::Options options_;
  std::unique_ptr<leveldb::DB> database_;
  std::array<Snapshot, test_support::SnapshotSlots> snapshots_;
};

class ReferenceMemoryFile final : public leveldb::WritableFile {
 public:
  leveldb::Status Append(const leveldb::Slice& bytes) override {
    contents.append(bytes.data(), bytes.size());
    return {};
  }
  leveldb::Status Close() override { return {}; }
  leveldb::Status Flush() override { return {}; }
  leveldb::Status Sync() override { return {}; }
  std::string contents;
};

TEST(LevelDbCompatibilityTest, ReferenceReallyEnablesBothCodecs) {
  for (const auto compression : {leveldb::kSnappyCompression, leveldb::kZstdCompression}) {
    leveldb::Options options;
    options.compression = compression;
    options.block_size = 16;
    ReferenceMemoryFile file;
    leveldb::TableBuilder builder(options, &file);
    builder.Add("key", std::string(4096, 'x'));
    CheckReference(builder.status());
    ASSERT_GT(file.contents.size(), 5U);
    EXPECT_EQ(static_cast<unsigned char>(file.contents[file.contents.size() - 5]), compression);
    CheckReference(builder.Finish());
  }
}

TEST(LevelDbCompatibilityTest, MatchesTheModelAndSwapsDatabaseDirectories) {
  for (const auto compression : {Compression::None, Compression::Snappy, Compression::Zstd}) {
    SCOPED_TRACE(static_cast<int>(compression));
    test_support::TemporaryDirectory root;
    const auto modern_path = root.path() / "modern";
    const auto reference_path = root.path() / "reference";
    Options options;
    options.create_if_missing = true;
    options.write_buffer_size = 64 * 1024;
    options.compression = compression;
    test_support::Model expected;
    {
      test_support::ModernClient modern(modern_path, options);
      ReferenceClient reference(reference_path, compression);
      expected = test_support::RunModel(20260926, 500, modern, reference);
    }
    {
      test_support::ModernClient modern(reference_path, options);
      ReferenceClient reference(modern_path, compression);
      EXPECT_EQ(modern.Scan(false), test_support::ExpectedEntries(expected, false));
      EXPECT_EQ(reference.Scan(true), test_support::ExpectedEntries(expected, true));
      for (const auto& [key, value] : expected) {
        EXPECT_EQ(modern.Read(key), value);
        EXPECT_EQ(reference.Read(key), value);
      }
    }
  }
}

}  // namespace
}  // namespace modern_leveldb
