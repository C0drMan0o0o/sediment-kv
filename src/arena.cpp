#include "sediment/arena.h"

#include <cassert>
#include <cstdint>

namespace sediment {

char* Arena::Allocate(size_t bytes) {
  return AllocateAligned(bytes, alignof(std::max_align_t));
}

char* Arena::AllocateAligned(size_t bytes, size_t alignment) {
  assert(alignment != 0 && (alignment & (alignment - 1)) == 0);
  const size_t mod = reinterpret_cast<uintptr_t>(ptr_) & (alignment - 1);
  const size_t slop = mod == 0 ? 0 : alignment - mod;
  if (bytes + slop <= remaining_) {
    char* result = ptr_ + slop;
    ptr_ += bytes + slop;
    remaining_ -= bytes + slop;
    return result;
  }
  return AllocateFallback(bytes, alignment);
}

char* Arena::AllocateFallback(size_t bytes, size_t alignment) {
  // Large requests get a dedicated block so the current block's tail is not
  // wasted. Over-allocate by `alignment` so the result can always be aligned.
  if (bytes + alignment > kBlockSize / 4) {
    char* block = NewBlock(bytes + alignment);
    const size_t mod = reinterpret_cast<uintptr_t>(block) & (alignment - 1);
    return block + (mod == 0 ? 0 : alignment - mod);
  }
  ptr_ = NewBlock(kBlockSize);
  remaining_ = kBlockSize;
  return AllocateAligned(bytes, alignment);
}

char* Arena::NewBlock(size_t block_bytes) {
  blocks_.push_back(std::make_unique<char[]>(block_bytes));
  memory_usage_ += block_bytes + sizeof(std::unique_ptr<char[]>);
  return blocks_.back().get();
}

}  // namespace sediment
