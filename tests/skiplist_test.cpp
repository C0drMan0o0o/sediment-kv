#include "sediment/skiplist.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <set>
#include <vector>

namespace sediment {
namespace {

struct IntCmp {
  int operator()(uint64_t a, uint64_t b) const {
    return a < b ? -1 : (a > b ? 1 : 0);
  }
};

using IntList = SkipList<uint64_t, IntCmp>;

TEST(SkipList, EmptyContainsNothing) {
  Arena arena;
  IntList list(IntCmp(), &arena);
  EXPECT_FALSE(list.Contains(0));
  EXPECT_FALSE(list.Contains(42));
}

TEST(SkipList, ContainsInsertedKeysOnly) {
  Arena arena;
  IntList list(IntCmp(), &arena);
  for (uint64_t k : {10, 5, 20, 1, 15}) {
    list.Insert(k);
  }
  for (uint64_t k : {1, 5, 10, 15, 20}) {
    EXPECT_TRUE(list.Contains(k));
  }
  for (uint64_t k : {0, 2, 11, 21}) {
    EXPECT_FALSE(list.Contains(k));
  }
}

TEST(SkipList, TenThousandRandomKeys) {
  Arena arena;
  IntList list(IntCmp(), &arena, /*seed=*/301);
  std::mt19937_64 rng(7);
  std::set<uint64_t> keys;
  while (keys.size() < 10000) {
    keys.insert(rng());
  }
  for (uint64_t k : keys) {
    list.Insert(k);
  }
  for (uint64_t k : keys) {
    EXPECT_TRUE(list.Contains(k));
  }
  for (int i = 0; i < 10000; ++i) {
    uint64_t probe = rng();
    EXPECT_EQ(list.Contains(probe), keys.count(probe) == 1);
  }
}

TEST(SkipList, SequentialAndReverseInsertion) {
  Arena arena;
  IntList list(IntCmp(), &arena);
  for (uint64_t k = 0; k < 2000; ++k) {
    list.Insert(k);
  }
  for (uint64_t k = 4000; k > 2000; --k) {
    list.Insert(k);
  }
  for (uint64_t k = 0; k < 2000; ++k) {
    EXPECT_TRUE(list.Contains(k));
  }
  for (uint64_t k = 2001; k <= 4000; ++k) {
    EXPECT_TRUE(list.Contains(k));
  }
  EXPECT_FALSE(list.Contains(2000));
}

TEST(SkipList, SameSeedGivesSameShape) {
  Arena a, b;
  IntList la(IntCmp(), &a, 99), lb(IntCmp(), &b, 99);
  for (uint64_t k = 0; k < 500; ++k) {
    la.Insert(k);
    lb.Insert(k);
  }
  EXPECT_EQ(a.MemoryUsage(), b.MemoryUsage());
}

}  // namespace
}  // namespace sediment
