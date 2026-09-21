#include "memory/arena.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace modern_leveldb {

MutableByteView Arena::Allocate(std::size_t bytes) {
  if (bytes == 0) {
    return {};
  }
  if (bytes <= bytes_remaining_) {
    std::byte* result = allocation_pointer_;
    allocation_pointer_ += bytes;
    bytes_remaining_ -= bytes;
    return {result, bytes};
  }
  return {AllocateFallback(bytes), bytes};
}

MutableByteView Arena::AllocateAligned(std::size_t bytes) {
  if (bytes == 0) {
    return {};
  }

  constexpr std::size_t Alignment = alignof(std::max_align_t);
  static_assert((Alignment & (Alignment - 1U)) == 0U);

  std::size_t padding = 0;
  if (allocation_pointer_ != nullptr) {
    const auto address = reinterpret_cast<std::uintptr_t>(allocation_pointer_);
    const std::size_t misalignment = address & (Alignment - 1U);
    padding = misalignment == 0U ? 0U : Alignment - misalignment;
  }

  if (padding <= bytes_remaining_ && bytes <= bytes_remaining_ - padding) {
    std::byte* result = allocation_pointer_ + padding;
    allocation_pointer_ += padding + bytes;
    bytes_remaining_ -= padding + bytes;
    assert(reinterpret_cast<std::uintptr_t>(result) % Alignment == 0U);
    return {result, bytes};
  }

  std::byte* result = AllocateFallback(bytes);
  assert(reinterpret_cast<std::uintptr_t>(result) % Alignment == 0U);
  return {result, bytes};
}

std::byte* Arena::AllocateFallback(std::size_t bytes) {
  if (bytes > DedicatedAllocationThreshold) {
    return AllocateBlock(bytes);
  }

  allocation_pointer_ = AllocateBlock(BlockSize);
  bytes_remaining_ = BlockSize;

  std::byte* result = allocation_pointer_;
  allocation_pointer_ += bytes;
  bytes_remaining_ -= bytes;
  return result;
}

std::byte* Arena::AllocateBlock(std::size_t bytes) {
  auto block = std::unique_ptr<std::byte[]>(new std::byte[bytes]);
  std::byte* result = block.get();
  blocks_.push_back(std::move(block));
  memory_usage_ += bytes;
  return result;
}

}  // namespace modern_leveldb
