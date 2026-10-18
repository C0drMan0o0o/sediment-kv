#include "sediment/arena.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace sediment {
namespace {

TEST(Arena, StartsEmpty) {
  Arena arena;
  EXPECT_EQ(arena.MemoryUsage(), 0u);
}

TEST(Arena, DefaultAllocationsAreMaxAligned) {
  Arena arena;
  for (size_t n : {1, 3, 7, 13, 100}) {
    auto p = reinterpret_cast<uintptr_t>(arena.Allocate(n));
    EXPECT_EQ(p % alignof(std::max_align_t), 0u);
  }
}

TEST(Arena, HonoursRequestedAlignment) {
  Arena arena;
  for (size_t align : {1, 2, 4, 8, 16, 64, 256}) {
    arena.Allocate(1);  // perturb the bump pointer
    auto p = reinterpret_cast<uintptr_t>(arena.AllocateAligned(5, align));
    EXPECT_EQ(p % align, 0u) << "alignment " << align;
  }
}

TEST(Arena, LargeAllocationsAreUsableAndAligned) {
  Arena arena;
  char* p = arena.AllocateAligned(100000, 64);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 64, 0u);
  std::memset(p, 0xAB, 100000);
  EXPECT_GE(arena.MemoryUsage(), 100000u);
}

TEST(Arena, AllocationsDoNotOverlap) {
  Arena arena;
  std::vector<std::pair<char*, size_t>> allocs;
  for (size_t i = 1; i < 2000; ++i) {
    size_t n = (i * 37) % 1500 + 1;
    char* p = arena.Allocate(n);
    std::memset(p, static_cast<int>(i & 0xFFU), n);
    allocs.emplace_back(p, n);
  }
  for (size_t i = 0; i < allocs.size(); ++i) {
    auto [p, n] = allocs[i];
    for (size_t j = 0; j < n; ++j) {
      ASSERT_EQ(static_cast<unsigned char>(p[j]), (i + 1) & 0xFFU);
    }
  }
}

TEST(Arena, UsageGrowsWithAllocations) {
  Arena arena;
  arena.Allocate(10);
  size_t first = arena.MemoryUsage();
  EXPECT_GE(first, 10u);
  for (int i = 0; i < 100; ++i) {
    arena.Allocate(1000);
  }
  EXPECT_GE(arena.MemoryUsage(), 100000u);
}

TEST(Arena, SmallAllocationsShareABlock) {
  Arena arena;
  arena.Allocate(8);
  size_t usage = arena.MemoryUsage();
  arena.Allocate(8);
  EXPECT_EQ(arena.MemoryUsage(), usage);
}

}  // namespace
}  // namespace sediment
