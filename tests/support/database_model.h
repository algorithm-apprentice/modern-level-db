#ifndef MODERN_LEVELDB_TESTS_SUPPORT_DATABASE_MODEL_H_
#define MODERN_LEVELDB_TESTS_SUPPORT_DATABASE_MODEL_H_

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

enum class ModelOperationKind : std::uint8_t {
    Put,
    Delete,
    Batch,
    Snapshot,
    Release,
    Read,
    Reopen,
    Scan,
};

struct ModelOperation {
    std::size_t index = 0;
    ModelOperationKind kind = ModelOperationKind::Put;
    std::string key;
    std::string value;
    ModelBatch batch;
    std::size_t slot = 0;
    bool sync = false;
    bool use_snapshot = false;

    friend bool operator==(const ModelOperation&, const ModelOperation&) = default;
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

class ModelWorkload final {
public:
    explicit ModelWorkload(std::uint64_t seed) : random_(seed) {}

    ModelOperation Next(std::size_t index) {
        ModelOperation operation;
        operation.index = index;
        operation.key = ModelKey(random_());
        operation.slot = static_cast<std::size_t>(random_() % SnapshotSlots);
        switch (random_() % 10) {
            case 0:
            case 1:
            case 2:
                operation.kind = ModelOperationKind::Put;
                operation.value = ModelValue(random_);
                break;
            case 3:
                operation.kind = ModelOperationKind::Delete;
                break;
            case 4:
                operation.kind = ModelOperationKind::Batch;
                operation.batch = ModelBatch{{operation.key, ModelValue(random_)},
                                             {ModelKey(random_()), std::nullopt},
                                             {operation.key, ModelValue(random_)}};
                operation.sync = index % 17 == 0;
                break;
            case 5:
                operation.kind = ModelOperationKind::Snapshot;
                break;
            case 6:
                operation.kind = ModelOperationKind::Release;
                break;
            case 7:
                operation.kind = ModelOperationKind::Read;
                break;
            case 8:
                operation.kind = ModelOperationKind::Reopen;
                break;
            case 9:
                operation.kind = ModelOperationKind::Scan;
                break;
        }
        return operation;
    }

private:
    std::mt19937_64 random_;
};

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

inline std::string ModelContext(std::uint64_t seed, std::string_view operation) {
    return "seed=" + std::to_string(seed) + " operation=" + std::string(operation) + ": ";
}

inline std::string ModelContext(std::uint64_t seed, std::size_t operation) {
    return ModelContext(seed, std::to_string(operation));
}

template <typename Left, typename Right>
void RequireModelEqual(const Left& actual, const Right& expected, std::string_view message) {
    if (actual != expected) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Observer, typename... Clients>
Model RunModelObserved(std::uint64_t seed, std::size_t operations, Observer&& observer,
                       Clients&... clients) {
    ModelWorkload workload(seed);
    Model model;
    std::array<std::optional<Model>, SnapshotSlots> snapshots;
    const auto each = [&](auto action) { (action(clients), ...); };
    for (std::size_t index = 0; index < operations; ++index) {
        ModelOperation operation = workload.Next(index);
        if (operation.kind == ModelOperationKind::Read) {
            operation.use_snapshot = snapshots[operation.slot].has_value();
        }
        try {
            observer(operation);
            switch (operation.kind) {
                case ModelOperationKind::Put:
                    each([&](auto& client) { client.Put(operation.key, operation.value); });
                    model[operation.key] = operation.value;
                    break;
                case ModelOperationKind::Delete:
                    each([&](auto& client) { client.Delete(operation.key); });
                    model.erase(operation.key);
                    break;
                case ModelOperationKind::Batch:
                    each([&](auto& client) { client.Write(operation.batch, operation.sync); });
                    for (const auto& [batch_key, value] : operation.batch) {
                        if (value.has_value()) {
                            model[batch_key] = *value;
                        } else {
                            model.erase(batch_key);
                        }
                    }
                    break;
                case ModelOperationKind::Snapshot:
                    each([&](auto& client) { client.SnapshotAt(operation.slot); });
                    snapshots[operation.slot] = model;
                    break;
                case ModelOperationKind::Release:
                    each([&](auto& client) { client.Release(operation.slot); });
                    snapshots[operation.slot].reset();
                    break;
                case ModelOperationKind::Read: {
                    const Model& view =
                        operation.use_snapshot ? *snapshots[operation.slot] : model;
                    const std::size_t selected =
                        operation.use_snapshot ? operation.slot : SnapshotSlots;
                    const auto found = view.find(operation.key);
                    std::optional<std::string> expected;
                    if (found != view.end()) {
                        expected = found->second;
                    }
                    each([&](auto& client) {
                        RequireModelEqual(client.Read(operation.key, selected), expected,
                                          "point-read result differs from model");
                    });
                    each([&](auto& client) {
                        RequireModelEqual(client.Scan(false, selected, operation.key),
                                          ExpectedEntries(view, false, operation.key),
                                          "seek scan differs from model");
                    });
                    break;
                }
                case ModelOperationKind::Reopen:
                    each([&](auto& client) {
                        client.Reopen();
                        RequireModelEqual(client.Scan(false), ExpectedEntries(model, false),
                                          "reopened state differs from model");
                    });
                    for (auto& snapshot : snapshots) {
                        snapshot.reset();
                    }
                    break;
                case ModelOperationKind::Scan:
                    each([&](auto& client) {
                        RequireModelEqual(client.Scan(false), ExpectedEntries(model, false),
                                          "forward scan differs from model");
                        RequireModelEqual(client.Scan(true), ExpectedEntries(model, true),
                                          "reverse scan differs from model");
                    });
                    break;
            }
        } catch (const std::exception& error) {
            throw std::runtime_error(ModelContext(seed, index) + error.what());
        } catch (...) {
            throw std::runtime_error(ModelContext(seed, index) + "unknown failure");
        }
    }
    try {
        each([&](auto& client) {
            RequireModelEqual(client.Scan(false), ExpectedEntries(model, false),
                              "final forward scan differs from model");
            RequireModelEqual(client.Scan(true), ExpectedEntries(model, true),
                              "final reverse scan differs from model");
        });
    } catch (const std::exception& error) {
        throw std::runtime_error(ModelContext(seed, "final") + error.what());
    } catch (...) {
        throw std::runtime_error(ModelContext(seed, "final") + "unknown failure");
    }
    return model;
}

template <typename... Clients>
Model RunModel(std::uint64_t seed, std::size_t operations, Clients&... clients) {
    return RunModelObserved(seed, operations, [](const ModelOperation&) {}, clients...);
}

}  // namespace modern_leveldb::test_support

#endif  // MODERN_LEVELDB_TESTS_SUPPORT_DATABASE_MODEL_H_
