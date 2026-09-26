#include "modern_leveldb/snapshot.h"

#include <memory>
#include <utility>

#include "api/api_internal.h"

namespace modern_leveldb {

Snapshot::Snapshot(std::shared_ptr<detail::SnapshotRegistration> registration) noexcept
    : registration_(std::move(registration)) {}

Snapshot::Snapshot(Snapshot&& source) noexcept = default;

Snapshot& Snapshot::operator=(Snapshot&& source) noexcept = default;

Snapshot::~Snapshot() = default;

}  // namespace modern_leveldb
