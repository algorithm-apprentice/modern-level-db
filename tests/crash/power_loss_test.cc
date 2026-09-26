#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/database.h"
#include "format/write_batch.h"
#include "support/crash_file_system.h"
#include "support/database_model.h"
#include "support/manual_clock.h"
#include "support/manual_executor.h"

namespace modern_leveldb {
namespace {

using test_support::Check;
using test_support::CrashFileSystem;
using test_support::CrashImage;
using test_support::Model;
using test_support::Take;

class CrashDatabase final {
 public:
  explicit CrashDatabase(const CrashImage& image = {}) : file_system(image) {}
  ~CrashDatabase() { Close(); }

  Status Open(bool create, BlockCompression compression = BlockCompression::None) {
    DatabaseEngineOptions options;
    options.file_system = &file_system;
    options.executor = &executor;
    options.clock = &clock;
    options.create_if_missing = create;
    options.write_buffer_size = 64 * 1024;
    options.table_options.compression = compression;
    auto opened = DatabaseEngine::Open(options, "db");
    if (!opened.has_value()) {
      return std::unexpected(opened.error());
    }
    database = std::move(*opened);
    return {};
  }
  void Close() {
    executor.RunAll();
    database.reset();
  }

  Model ReadAll() {
    auto iterator = database->NewIterator();
    Check(iterator->SeekToFirst());
    Model model;
    while (iterator->valid()) {
      const bool inserted =
          model.emplace(AsStringView(iterator->key()), AsStringView(iterator->value())).second;
      if (!inserted || model.size() > 32) {
        throw std::runtime_error("recovered iterator repeats keys");
      }
      Check(iterator->Next());
    }
    return model;
  }

  CrashFileSystem file_system;
  test_support::ManualExecutor executor;
  test_support::ManualClock clock;
  std::unique_ptr<DatabaseEngine> database;
};

TEST(CrashFileSystemTest, SyncingNamesDoesNotSyncBytesAndFlushDoesNotPersist) {
  CrashFileSystem fs;
  Check(fs.CreateDirectory("db"));
  Check(fs.SyncDirectory("."));
  auto file = Take(fs.OpenWritable("db/data"));
  Check(file->Append(AsBytes("first")));
  Check(fs.SyncDirectory("db"));
  EXPECT_TRUE(fs.DurableImage().files.at("db/data").empty());
  Check(file->Sync());
  Check(file->Append(AsBytes("second")));
  Check(file->Flush());
  Check(file->Close());
  EXPECT_EQ(AsStringView(fs.DurableImage().files.at("db/data")), "first");
}

TEST(CrashFileSystemTest, UnsyncedReplacementAndRemovalPreserveDurableNames) {
  CrashFileSystem fs;
  Check(fs.CreateDirectory("db"));
  Check(fs.SyncDirectory("."));
  auto original = Take(fs.OpenWritable("db/CURRENT"));
  Check(original->Append(AsBytes("old")));
  Check(original->Sync());
  Check(original->Close());
  Check(fs.SyncDirectory("db"));

  auto replacement = Take(fs.OpenWritable("db/new"));
  Check(replacement->Append(AsBytes("new")));
  Check(replacement->Sync());
  Check(replacement->Close());
  Check(fs.RenameFile("db/new", "db/CURRENT"));
  EXPECT_EQ(AsStringView(fs.DurableImage().files.at("db/CURRENT")), "old");
  Check(fs.SyncDirectory("db"));
  EXPECT_EQ(AsStringView(fs.DurableImage().files.at("db/CURRENT")), "new");
  Check(fs.RemoveFile("db/CURRENT"));
  EXPECT_TRUE(fs.DurableImage().files.contains("db/CURRENT"));
  Check(fs.SyncDirectory("db"));
  EXPECT_FALSE(fs.DurableImage().files.contains("db/CURRENT"));
}

TEST(CrashFileSystemTest, PowerLossFreezesLaterSyncAndCleanup) {
  CrashFileSystem fs;
  Check(fs.CreateDirectory("db"));
  Check(fs.SyncDirectory("."));
  auto file = Take(fs.OpenWritable("db/data"));
  Check(file->Append(AsBytes("unsynced")));
  fs.CrashAt(fs.mutation_count());
  EXPECT_FALSE(file->Sync().has_value());
  EXPECT_TRUE(fs.crashed());
  EXPECT_FALSE(fs.SyncDirectory("db").has_value());
  EXPECT_FALSE(file->Close().has_value());
  EXPECT_TRUE(fs.DurableImage().files.empty());
}

CrashImage Baseline() {
  CrashDatabase base;
  Check(base.Open(true));
  EncodedWriteBatch batch;
  Check(batch.Put(AsBytes("baseline"), AsBytes("durable")));
  Check(base.database->Write(batch, true));
  base.Close();
  return base.file_system.DurableImage();
}

struct Outcome {
  CrashImage image;
  std::vector<Model> possible{{{"baseline", "durable"}}};
  std::size_t acknowledged = 0;
  std::size_t mutations = 0;
  bool crashed = false;
};

Outcome RunCrashTrace(const CrashImage& base, BlockCompression compression,
                      std::optional<std::size_t> cut) {
  CrashDatabase run(base);
  run.file_system.CrashAt(cut);
  Outcome outcome;
  const Status opened = run.Open(false, compression);
  if (opened.has_value()) {
    for (unsigned batch_index = 0; batch_index < 8; ++batch_index) {
      const std::string value(40 * 1024, static_cast<char>('a' + batch_index));
      EncodedWriteBatch batch;
      Check(batch.Put(AsBytes("a"), AsBytes(value)));
      Check(batch.Put(AsBytes("b"), AsBytes(value)));
      const std::string marker = "batch/" + std::to_string(batch_index);
      Check(batch.Put(AsBytes(marker), AsBytes("committed")));
      Model next = outcome.possible.back();
      next["a"] = value;
      next["b"] = value;
      next[marker] = "committed";
      if (batch_index % 2 == 0) {
        Check(batch.Delete(AsBytes("baseline")));
        next.erase("baseline");
      } else {
        Check(batch.Put(AsBytes("baseline"), AsBytes("durable")));
        next["baseline"] = "durable";
      }
      outcome.possible.push_back(std::move(next));
      const bool sync = batch_index != 7;
      const Status written = run.database->Write(batch, sync);
      if (!written.has_value()) {
        if (!run.file_system.crashed()) {
          Check(written);
        }
        break;
      }
      if (sync) {
        outcome.acknowledged = outcome.possible.size() - 1;
      }
      run.executor.RunAll();
      if (run.file_system.crashed()) {
        break;
      }
      Check(run.database->WaitForBackgroundWork());
    }
  } else if (!run.file_system.crashed()) {
    Check(opened);
  }
  run.Close();
  outcome.image = run.file_system.DurableImage();
  outcome.mutations = run.file_system.mutation_count();
  outcome.crashed = run.file_system.crashed();
  return outcome;
}

TEST(PowerLossTest, PreservesAcknowledgedBatchesAtEveryMutationBoundary) {
  const CrashImage base = Baseline();
  for (const BlockCompression compression :
       {BlockCompression::None, BlockCompression::Snappy, BlockCompression::Zstd}) {
    const Outcome complete = RunCrashTrace(base, compression, std::nullopt);
    ASSERT_FALSE(complete.crashed);
    ASSERT_EQ(complete.possible.size(), 9U);
    ASSERT_EQ(complete.acknowledged, 7U);
    ASSERT_GT(complete.mutations, 100U);
    for (std::size_t cut = 0; cut <= complete.mutations; ++cut) {
      SCOPED_TRACE(testing::Message()
                   << "compression=" << static_cast<int>(compression) << " mutation=" << cut);
      const Outcome stopped = RunCrashTrace(base, compression, cut);
      EXPECT_EQ(stopped.crashed, cut < complete.mutations);
      CrashDatabase recovered(stopped.image);
      Check(recovered.Open(false));
      const Model actual = recovered.ReadAll();
      bool matched = false;
      for (std::size_t state = stopped.acknowledged; state < stopped.possible.size(); ++state) {
        matched = matched || actual == stopped.possible[state];
      }
      EXPECT_TRUE(matched) << "recovery lost an acknowledged batch or exposed a partial batch";
    }
  }
}

}  // namespace
}  // namespace modern_leveldb
