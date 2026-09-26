#ifndef MODERN_LEVELDB_TESTS_SUPPORT_DATABASE_MODEL_H_
#define MODERN_LEVELDB_TESTS_SUPPORT_DATABASE_MODEL_H_

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "modern_leveldb/db.h"

namespace modern_leveldb::test_support {

using Model = std::map<std::string, std::string>;
using Entries = std::vector<std::pair<std::string, std::string>>;
using ModelBatch = std::vector<std::pair<std::string, std::optional<std::string>>>;
inline constexpr std::size_t SnapshotSlots = 4;

inline void Check(const Status& status) {
  if (!status.has_value()) {
    throw std::runtime_error(status.error().ToString());
  }
}

template <typename T>
T Take(Result<T> result) {
  if (!result.has_value()) {
    throw std::runtime_error(result.error().ToString());
  }
  return std::move(*result);
}

inline Entries ExpectedEntries(const Model& model, bool reverse,
                               std::optional<std::string_view> target = {}) {
  const auto begin = target.has_value() ? model.lower_bound(std::string(*target)) : model.begin();
  Entries result(begin, model.end());
  if (reverse) {
    std::reverse(result.begin(), result.end());
  }
  return result;
}

class ModernClient final {
 public:
  ModernClient(std::filesystem::path path, Options options)
      : path_(std::move(path)), options_(std::move(options)) {
    Open();
  }
  ModernClient(const ModernClient&) = delete;
  ModernClient& operator=(const ModernClient&) = delete;
  ModernClient(ModernClient&&) = delete;
  ModernClient& operator=(ModernClient&&) = delete;

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
    Check(database_->Put(AsBytes(key), AsBytes(value)));
  }
  void Delete(std::string_view key) { Check(database_->Delete(AsBytes(key))); }
  void Write(const ModelBatch& operations, bool sync) {
    WriteBatch batch;
    for (const auto& [key, value] : operations) {
      if (value.has_value()) {
        Check(batch.Put(AsBytes(key), AsBytes(*value)));
      } else {
        Check(batch.Delete(AsBytes(key)));
      }
    }
    Check(database_->Write(batch, {.sync = sync}));
  }
  void SnapshotAt(std::size_t slot) { snapshots_[slot].emplace(Take(database_->GetSnapshot())); }
  void Release(std::size_t slot) { snapshots_[slot].reset(); }

  std::optional<std::string> Read(std::string_view key, std::size_t slot = SnapshotSlots) {
    const auto value = Take(database_->Get(AsBytes(key), ReadAt(slot)));
    if (!value.has_value()) {
      return std::nullopt;
    }
    return std::string(AsStringView(*value));
  }

  Entries Scan(bool reverse, std::size_t slot = SnapshotSlots,
               std::optional<std::string_view> target = {}) {
    Iterator iterator = Take(database_->NewIterator(ReadAt(slot)));
    if (target.has_value()) {
      Check(iterator.Seek(AsBytes(*target)));
    } else if (reverse) {
      Check(iterator.SeekToLast());
    } else {
      Check(iterator.SeekToFirst());
    }
    Entries entries;
    while (iterator.valid()) {
      if (entries.size() > 4096) {
        throw std::runtime_error("database iterator did not terminate");
      }
      entries.emplace_back(AsStringView(iterator.key()), AsStringView(iterator.value()));
      if (reverse) {
        Check(iterator.Prev());
      } else {
        Check(iterator.Next());
      }
    }
    return entries;
  }

 private:
  void Open() { database_.emplace(Take(Database::Open(options_, path_))); }
  ReadOptions ReadAt(std::size_t slot) const {
    ReadOptions options;
    options.fill_cache = false;
    if (slot != SnapshotSlots) {
      options.snapshot = &snapshots_.at(slot).value();
    }
    return options;
  }

  std::filesystem::path path_;
  Options options_;
  std::optional<Database> database_;
  std::array<std::optional<Snapshot>, SnapshotSlots> snapshots_;
};

inline std::string ModelKey(std::uint64_t number) {
  return std::string("key\0", 4) + static_cast<char>(number % 32);
}

inline std::string ModelValue(std::mt19937_64& random) {
  const std::size_t length = static_cast<std::size_t>(random() % 2049);
  std::string result(length, static_cast<char>(random()));
  if (random() % 3 == 0) {
    for (char& byte : result) {
      byte = static_cast<char>(random());
    }
  }
  return result;
}

template <typename... Clients>
Model RunModel(std::uint64_t seed, std::size_t operations, Clients&... clients) {
  std::mt19937_64 random(seed);
  Model model;
  std::array<std::optional<Model>, SnapshotSlots> snapshots;
  const auto each = [&](auto action) { (action(clients), ...); };
  for (std::size_t index = 0; index < operations; ++index) {
    SCOPED_TRACE(testing::Message() << "seed=" << seed << " operation=" << index);
    const std::string key = ModelKey(random());
    const std::size_t slot = static_cast<std::size_t>(random() % SnapshotSlots);
    switch (random() % 10) {
      case 0:
      case 1:
      case 2: {
        const std::string value = ModelValue(random);
        each([&](auto& client) { client.Put(key, value); });
        model[key] = value;
        break;
      }
      case 3:
        each([&](auto& client) { client.Delete(key); });
        model.erase(key);
        break;
      case 4: {
        const ModelBatch batch{{key, ModelValue(random)},
                               {ModelKey(random()), std::nullopt},
                               {key, ModelValue(random)}};
        each([&](auto& client) { client.Write(batch, index % 17 == 0); });
        for (const auto& [batch_key, value] : batch) {
          if (value.has_value()) {
            model[batch_key] = *value;
          } else {
            model.erase(batch_key);
          }
        }
        break;
      }
      case 5:
        each([&](auto& client) { client.SnapshotAt(slot); });
        snapshots[slot] = model;
        break;
      case 6:
        each([&](auto& client) { client.Release(slot); });
        snapshots[slot].reset();
        break;
      case 7: {
        const bool use_snapshot = snapshots[slot].has_value();
        const Model& view = use_snapshot ? *snapshots[slot] : model;
        const std::size_t selected = use_snapshot ? slot : SnapshotSlots;
        const auto found = view.find(key);
        std::optional<std::string> expected;
        if (found != view.end()) {
          expected = found->second;
        }
        each([&](auto& client) { EXPECT_EQ(client.Read(key, selected), expected); });
        each([&](auto& client) {
          EXPECT_EQ(client.Scan(false, selected, key), ExpectedEntries(view, false, key));
        });
        break;
      }
      case 8:
        each([&](auto& client) {
          client.Reopen();
          EXPECT_EQ(client.Scan(false), ExpectedEntries(model, false));
        });
        for (auto& snapshot : snapshots) {
          snapshot.reset();
        }
        break;
      case 9:
        each([&](auto& client) {
          EXPECT_EQ(client.Scan(false), ExpectedEntries(model, false));
          EXPECT_EQ(client.Scan(true), ExpectedEntries(model, true));
        });
        break;
    }
  }
  each([&](auto& client) {
    EXPECT_EQ(client.Scan(false), ExpectedEntries(model, false));
    EXPECT_EQ(client.Scan(true), ExpectedEntries(model, true));
  });
  return model;
}

}  // namespace modern_leveldb::test_support

#endif  // MODERN_LEVELDB_TESTS_SUPPORT_DATABASE_MODEL_H_
