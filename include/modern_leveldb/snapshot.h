#ifndef MODERN_LEVELDB_SNAPSHOT_H_
#define MODERN_LEVELDB_SNAPSHOT_H_

#include <memory>

namespace modern_leveldb {

class Database;

namespace detail {
class SnapshotRegistration;
}

class Snapshot final {
 public:
  Snapshot(const Snapshot&) = delete;
  Snapshot& operator=(const Snapshot&) = delete;
  Snapshot(Snapshot&& source) noexcept;
  Snapshot& operator=(Snapshot&& source) noexcept;
  ~Snapshot();

 private:
  explicit Snapshot(std::shared_ptr<detail::SnapshotRegistration> registration) noexcept;

  friend class Database;

  std::shared_ptr<detail::SnapshotRegistration> registration_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_SNAPSHOT_H_
