#pragma once

#include <cstddef>
#include <vector>

namespace sediment {

// Block-based bump allocator. Memory is released all at once when the Arena
// is destroyed; individual allocations are never freed. Not thread-safe.
class Arena {
 public:
  Arena() = default;
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  // Return `bytes` of uninitialised memory aligned to alignof(max_align_t).
  char* Allocate(size_t bytes);
  // Like Allocate, but aligned to `alignment`, which must be a power of two.
  char* AllocateAligned(size_t bytes, size_t alignment);

  // Total bytes obtained from the system, including per-block slack.
  size_t MemoryUsage() const { return memory_usage_; }

  static constexpr size_t kBlockSize = 4096;

 private:
  char* NewBlock(size_t block_bytes);

  char* ptr_ = nullptr;
  size_t remaining_ = 0;
  std::vector<std::vector<char>> blocks_;
  size_t memory_usage_ = 0;
};

}  // namespace sediment
