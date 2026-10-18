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
  // A fixed seed keeps the test repeatable.
  // NOLINTNEXTLINE(bugprone-random-generator-seed)
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

TEST(SkipList, EmptyIteratorIsInvalid) {
  Arena arena;
  IntList list(IntCmp(), &arena);
  IntList::Iterator it(&list);
  EXPECT_FALSE(it.Valid());
  it.SeekToFirst();
  EXPECT_FALSE(it.Valid());
  it.Seek(5);
  EXPECT_FALSE(it.Valid());
}

TEST(SkipList, SeekFindsFirstGreaterOrEqual) {
  Arena arena;
  IntList list(IntCmp(), &arena);
  for (uint64_t k : {10, 20, 30}) {
    list.Insert(k);
  }
  IntList::Iterator it(&list);
  it.Seek(0);
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), 10u);
  it.Seek(20);
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), 20u);
  it.Seek(21);
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), 30u);
  it.Seek(31);
  EXPECT_FALSE(it.Valid());
}

// Fixed seeds: compares full iteration and Seek against a std::set oracle.
TEST(SkipList, RandomizedMatchesStdSetOracle) {
  for (uint32_t seed = 1; seed <= 20; ++seed) {
    Arena arena;
    IntList list(IntCmp(), &arena, seed);
    std::mt19937_64 rng(seed);
    std::set<uint64_t> oracle;
    const size_t n = 200 + seed * 100;
    while (oracle.size() < n) {
      uint64_t k = rng() % (n * 4);
      if (oracle.insert(k).second) {
        list.Insert(k);
      }
    }

    IntList::Iterator it(&list);
    it.SeekToFirst();
    for (uint64_t expected : oracle) {
      ASSERT_TRUE(it.Valid()) << "seed " << seed;
      ASSERT_EQ(it.key(), expected) << "seed " << seed;
      it.Next();
    }
    EXPECT_FALSE(it.Valid());

    for (int i = 0; i < 1000; ++i) {
      uint64_t target = rng() % (n * 4 + 10);
      it.Seek(target);
      auto want = oracle.lower_bound(target);
      if (want == oracle.end()) {
        ASSERT_FALSE(it.Valid()) << "seed " << seed << " target " << target;
        continue;
      }
      // Walk a few steps to check order after Seek, not just the landing.
      for (int step = 0; step < 5 && want != oracle.end(); ++step, ++want) {
        ASSERT_TRUE(it.Valid()) << "seed " << seed << " target " << target;
        ASSERT_EQ(it.key(), *want);
        it.Next();
      }
    }
  }
}

}  // namespace
}  // namespace sediment
