#include "sediment/arena.h"

#include <cassert>
#include <cstdint>

namespace sediment {
namespace {

char* AlignUp(char* p, size_t alignment) {
  const size_t mod = reinterpret_cast<uintptr_t>(p) & (alignment - 1);
  return mod == 0 ? p : p + (alignment - mod);
}

}  // namespace

char* Arena::Allocate(size_t bytes) {
  return AllocateAligned(bytes, alignof(std::max_align_t));
}

char* Arena::AllocateAligned(size_t bytes, size_t alignment) {
  assert(alignment != 0 && (alignment & (alignment - 1)) == 0);
  // Large requests get a dedicated block so the current block's tail is not
  // wasted. Over-allocate by `alignment` so the result can always be aligned.
  if (bytes + alignment > kBlockSize / 4) {
    return AlignUp(NewBlock(bytes + alignment), alignment);
  }
  size_t slop = AlignUp(ptr_, alignment) - ptr_;
  if (bytes + slop > remaining_) {
    ptr_ = NewBlock(kBlockSize);
    remaining_ = kBlockSize;
    slop = AlignUp(ptr_, alignment) - ptr_;
  }
  char* result = ptr_ + slop;
  ptr_ += bytes + slop;
  remaining_ -= bytes + slop;
  return result;
}

char* Arena::NewBlock(size_t block_bytes) {
  blocks_.emplace_back(block_bytes);
  memory_usage_ += block_bytes + sizeof(std::vector<char>);
  return blocks_.back().data();
}

}  // namespace sediment
