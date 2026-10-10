#ifndef MODERN_LEVELDB_WRITE_BATCH_H_
#define MODERN_LEVELDB_WRITE_BATCH_H_

#include <cstddef>
#include <memory>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

class Database;

// Owns an ordered list of updates; the engine assigns sequences on submission.
// Copying duplicates the operations, moving transfers them. Mutations copy their
// input bytes. A batch is mutable and needs external synchronization when shared.
// See docs/learning/02-bytes-and-formats.md for the representation.
class WriteBatch final {
public:
    WriteBatch();
    WriteBatch(const WriteBatch& source);
    WriteBatch& operator=(const WriteBatch& source);
    WriteBatch(WriteBatch&& source) noexcept;
    WriteBatch& operator=(WriteBatch&& source) noexcept;
    ~WriteBatch();

    // Empty keys/values are valid; lengths follow the persistent uint32 representation.
    // Delete is distinct from Put with an empty value. Fallible operations reject a
    // moved-from batch. Clear is a no-op and ApproximateSize returns zero in that state.
    [[nodiscard]] Status Put(ByteView key, ByteView value);
    [[nodiscard]] Status Delete(ByteView key);
    [[nodiscard]] Status Append(const WriteBatch& source);
    void Clear() noexcept;
    [[nodiscard]] std::size_t ApproximateSize() const noexcept;

private:
    class Impl;

    friend class Database;

    std::unique_ptr<Impl> impl_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_WRITE_BATCH_H_
