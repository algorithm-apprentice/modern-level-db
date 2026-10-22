#ifndef MODERN_LEVELDB_DB_H_
#define MODERN_LEVELDB_DB_H_

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"
#include "modern_leveldb/database_state_snapshot.h"
#include "modern_leveldb/iterator.h"
#include "modern_leveldb/options.h"
#include "modern_leveldb/snapshot.h"
#include "modern_leveldb/write_batch.h"

namespace modern_leveldb {

namespace detail {
class DatabaseState;
}

using OwningReadResult = Result<std::optional<std::vector<std::byte>>>;

// Owns access to one exclusively locked database directory. Concurrent operations
// are supported, but moving or destroying this handle must not race with its use.
// Snapshots and iterators retain the engine and can keep the directory locked
// after this handle is destroyed. See docs/learning/08-cpp-ownership-errors-and-concurrency.md.
class Database final {
public:
    // Opens an existing database, or creates one when allowed by options.
    // A custom comparator's ordering identity must remain stable across reopens.
    // Platform/durability limits are in docs/reference/platform-support-and-durability.md.
    [[nodiscard]] static Result<Database> Open(Options options, std::filesystem::path directory);

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&& source) noexcept;
    Database& operator=(Database&& source) noexcept;
    ~Database();

    // Input bytes are borrowed during the call and copied into the write batch;
    // key/value lengths follow its persistent uint32 representation. A failure does
    // not prove the update is absent after recovery: WAL bytes may already have
    // reached storage. Request sync for the backend's data barrier.
    [[nodiscard]] Status Put(ByteView key, ByteView value, const WriteOptions& options = {});
    [[nodiscard]] Status Delete(ByteView key, const WriteOptions& options = {});
    // Atomically publishes all batch operations. Copies the batch, so the caller
    // may share or inspect it during the call, but must not mutate it concurrently.
    [[nodiscard]] Status Write(const WriteBatch& batch, const WriteOptions& options = {});
    // Exclusively borrows the batch until this call returns. No thread may read,
    // copy, mutate, or submit it concurrently; its public contents are unchanged.
    [[nodiscard]] Status WriteExclusive(WriteBatch& batch, const WriteOptions& options = {});
    // Returns true and replaces value when found. Missing/deleted keys return
    // false without changing value; read failures are errors, not absence.
    [[nodiscard]] Result<bool> Get(ByteView key, std::vector<std::byte>& value,
                                   const ReadOptions& options = {});
    // The owning alternative: an empty optional means missing/deleted, and an
    // engaged vector can itself be empty for a stored empty value.
    [[nodiscard]] OwningReadResult Get(ByteView key, const ReadOptions& options = {});
    // Synchronous, ordered owning reads at one retained sequence. Inputs remain
    // stable until return; duplicates and per-key errors retain their positions.
    // Valid concurrency is 1..16; one runs on the caller without read workers.
    // All workers finish before return or propagation of an exception.
    [[nodiscard]] Result<std::vector<OwningReadResult>> MultiGet(
        std::span<const ByteView> keys, const MultiGetOptions& options = {});
    // Captures a fixed read sequence and starts unpositioned. A supplied snapshot
    // registration is retained even after its public Snapshot handle is destroyed.
    [[nodiscard]] Result<Iterator> NewIterator(const ReadOptions& options = {});
    // Pins the current read sequence, not a copy of database files or values.
    [[nodiscard]] Result<Snapshot> GetSnapshot();
    // Copies topology/maintenance state without I/O or waiting for background work.
    // This observation is not a visibility pin and cannot be passed to ReadOptions.
    [[nodiscard]] Result<DatabaseStateSnapshot> GetState();

private:
    explicit Database(std::shared_ptr<detail::DatabaseState> state) noexcept;

    std::shared_ptr<detail::DatabaseState> state_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_DB_H_
