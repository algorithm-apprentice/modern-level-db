#include "memory/arena.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace modern_leveldb {

std::byte* Arena::Allocate(std::size_t bytes) {
    if (bytes <= bytes_remaining_) {
        return AllocateFromCurrentBlock(bytes);
    }
    return AllocateFallback(bytes);
}

std::byte* Arena::AllocateAligned(std::size_t bytes) {
    static_assert((Alignment & (Alignment - 1U)) == 0U);

    const std::size_t padding = AlignmentPadding(allocation_pointer_);
    if (CanAllocateFromCurrentBlock(bytes, padding)) {
        std::byte* result = AllocateFromCurrentBlock(bytes, padding);
        return result;
    }

    std::byte* result = AllocateFallback(bytes);
    return result;
}

std::size_t Arena::AlignmentPadding(const std::byte* pointer) noexcept {
    if (pointer == nullptr) {
        return 0;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    const std::size_t misalignment = address & (Alignment - 1U);
    return misalignment == 0U ? 0U : Alignment - misalignment;
}

bool Arena::CanAllocateFromCurrentBlock(std::size_t bytes, std::size_t padding) const noexcept {
    return bytes <= bytes_remaining_ - padding;
}

bool Arena::ShouldAllocateDedicated(std::size_t bytes) noexcept {
    return bytes > DedicatedAllocationThreshold;
}

void Arena::RefillCurrentBlock() {
    allocation_pointer_ = AllocateBlock(BlockSize);
    bytes_remaining_ = BlockSize;
}

std::byte* Arena::AllocateFromCurrentBlock(std::size_t bytes, std::size_t padding) noexcept {
    std::byte* result = allocation_pointer_ + padding;
    allocation_pointer_ += padding + bytes;
    bytes_remaining_ -= padding + bytes;
    return result;
}

std::byte* Arena::AllocateFallback(std::size_t bytes) {
    // Large requests get a dedicated block without replacing the current small
    // allocation block, preserving its remaining space for subsequent entries.
    if (ShouldAllocateDedicated(bytes)) {
        return AllocateBlock(bytes);
    }

    RefillCurrentBlock();
    return AllocateFromCurrentBlock(bytes);
}

std::byte* Arena::AllocateBlock(std::size_t bytes) {
    auto block = std::unique_ptr<std::byte[]>(new std::byte[bytes]);
    std::byte* result = block.get();
    blocks_.push_back(std::move(block));
    memory_usage_ += bytes + sizeof(std::byte*);
    return result;
}

}  // namespace modern_leveldb
