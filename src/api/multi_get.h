#ifndef MODERN_LEVELDB_API_MULTI_GET_H_
#define MODERN_LEVELDB_API_MULTI_GET_H_

#include <functional>
#include <memory>
#include <span>
#include <thread>
#include <vector>

#include "engine/database.h"
#include "modern_leveldb/db.h"

namespace modern_leveldb::detail {

class DatabaseState;
class SnapshotRegistration;

[[nodiscard]] OwningReadResult ReadBatchKey(DatabaseEngine& engine, ByteView key,
                                            const DatabaseEngineReadOptions& options);
[[nodiscard]] std::jthread StartBatchWorker(std::function<void()> task);

// Concrete batch dependencies, separate from the serial maintenance executor.
// Start returns a joinable exactly-once worker or throws without retaining work.
// Read supports concurrent calls and returns owning per-key results.
struct MultiGetRuntime {
    std::function<OwningReadResult(DatabaseEngine&, ByteView, const DatabaseEngineReadOptions&)>
        read = &ReadBatchKey;
    std::function<std::jthread(std::function<void()>)> start = &StartBatchWorker;
};

// The facade validates state, concurrency and snapshot identity before entry.
// One implicit registration spans every key, including gaps between engine Gets.
[[nodiscard]] std::vector<OwningReadResult> ReadBatch(
    std::shared_ptr<DatabaseState> state, std::span<const ByteView> keys,
    DatabaseEngineReadOptions options, std::shared_ptr<SnapshotRegistration> snapshot,
    std::size_t max_concurrency, const MultiGetRuntime& runtime = {});

}  // namespace modern_leveldb::detail

#endif  // MODERN_LEVELDB_API_MULTI_GET_H_
