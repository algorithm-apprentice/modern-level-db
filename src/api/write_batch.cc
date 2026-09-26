#include "modern_leveldb/write_batch.h"

#include <memory>
#include <utility>

#include "api/api_internal.h"

namespace modern_leveldb {
namespace {

Status MovedFromBatch() {
  return std::unexpected(Error::InvalidArgument("the write batch was moved from"));
}

}  // namespace

WriteBatch::WriteBatch() : impl_(std::make_unique<Impl>()) {}

WriteBatch::WriteBatch(const WriteBatch& source)
    : impl_(source.impl_ != nullptr ? std::make_unique<Impl>(*source.impl_) : nullptr) {}

WriteBatch& WriteBatch::operator=(const WriteBatch& source) {
  if (this != &source) {
    WriteBatch replacement(source);
    impl_.swap(replacement.impl_);
  }
  return *this;
}

WriteBatch::WriteBatch(WriteBatch&& source) noexcept = default;

WriteBatch& WriteBatch::operator=(WriteBatch&& source) noexcept = default;

WriteBatch::~WriteBatch() = default;

Status WriteBatch::Put(ByteView key, ByteView value) {
  if (impl_ == nullptr) {
    return MovedFromBatch();
  }
  return impl_->batch().Put(key, value);
}

Status WriteBatch::Delete(ByteView key) {
  if (impl_ == nullptr) {
    return MovedFromBatch();
  }
  return impl_->batch().Delete(key);
}

Status WriteBatch::Append(const WriteBatch& source) {
  if (impl_ == nullptr || source.impl_ == nullptr) {
    return MovedFromBatch();
  }
  return impl_->batch().Append(source.impl_->batch());
}

void WriteBatch::Clear() noexcept {
  if (impl_ != nullptr) {
    impl_->batch().Clear();
  }
}

std::size_t WriteBatch::ApproximateSize() const noexcept {
  return impl_ != nullptr ? impl_->batch().encoded().size() : 0;
}

}  // namespace modern_leveldb
