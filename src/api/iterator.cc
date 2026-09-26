#include "modern_leveldb/iterator.h"

#include <cassert>
#include <memory>
#include <utility>

#include "api/api_internal.h"

namespace modern_leveldb {
namespace {

Status MovedFromIterator() {
  return std::unexpected(Error::InvalidArgument("the iterator was moved from"));
}

}  // namespace

Iterator::Iterator(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Iterator::Iterator(Iterator&& source) noexcept = default;

Iterator& Iterator::operator=(Iterator&& source) noexcept = default;

Iterator::~Iterator() = default;

bool Iterator::valid() const noexcept { return impl_ != nullptr && impl_->iterator().valid(); }

ByteView Iterator::key() const noexcept {
  assert(valid());
  return impl_->iterator().key();
}

ByteView Iterator::value() const noexcept {
  assert(valid());
  return impl_->iterator().value();
}

Status Iterator::SeekToFirst() {
  if (impl_ == nullptr) {
    return MovedFromIterator();
  }
  return impl_->iterator().SeekToFirst();
}

Status Iterator::SeekToLast() {
  if (impl_ == nullptr) {
    return MovedFromIterator();
  }
  return impl_->iterator().SeekToLast();
}

Status Iterator::Seek(ByteView key) {
  if (impl_ == nullptr) {
    return MovedFromIterator();
  }
  return impl_->iterator().Seek(key);
}

Status Iterator::Next() {
  if (impl_ == nullptr) {
    return MovedFromIterator();
  }
  return impl_->iterator().Next();
}

Status Iterator::Prev() {
  if (impl_ == nullptr) {
    return MovedFromIterator();
  }
  return impl_->iterator().Prev();
}

}  // namespace modern_leveldb
