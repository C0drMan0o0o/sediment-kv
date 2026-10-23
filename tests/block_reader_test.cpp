#include "sediment/block_reader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "sediment/block_builder.h"

namespace sediment {
namespace {

using Entries = std::vector<std::pair<std::string, std::string>>;

std::string Build(const Entries& entries, int restart_interval) {
  BlockBuilder builder(restart_interval);
  for (const auto& [k, v] : entries) {
    builder.Add(k, v);
  }
  return std::string(builder.Finish());
}

// Zero-padded keys share long prefixes, exercising prefix compression.
Entries SortedEntries(int n) {
  Entries entries;
  for (int i = 0; i < n; ++i) {
    char key[32];
    std::snprintf(key, sizeof(key), "key%08d", i * 2);  // gaps for misses
    entries.emplace_back(key, "value" + std::to_string(i));
  }
  return entries;
}

TEST(BlockReader, EmptyBlockHasNoEntries) {
  const std::string block = Build({}, 16);
  BlockReader reader(block);
  ASSERT_TRUE(reader.status().ok());
  auto it = reader.NewIterator();
  it.SeekToFirst();
  EXPECT_FALSE(it.Valid());
  EXPECT_TRUE(it.status().ok());
  it.Seek("anything");
  EXPECT_FALSE(it.Valid());
  EXPECT_TRUE(it.status().ok());
}

TEST(BlockReader, ScansAllEntriesInOrder) {
  for (const int interval : {1, 2, 16}) {
    const Entries entries = SortedEntries(100);
    const std::string block = Build(entries, interval);
    BlockReader reader(block);
    ASSERT_TRUE(reader.status().ok());
    auto it = reader.NewIterator();
    size_t i = 0;
    for (it.SeekToFirst(); it.Valid(); it.Next(), ++i) {
      ASSERT_LT(i, entries.size());
      EXPECT_EQ(it.key(), entries[i].first);
      EXPECT_EQ(it.value(), entries[i].second);
    }
    EXPECT_EQ(i, entries.size());
    EXPECT_TRUE(it.status().ok());
  }
}

TEST(BlockReader, SeekLandsOnFirstKeyAtOrAfterTarget) {
  const Entries entries = SortedEntries(50);
  const std::string block = Build(entries, 4);
  BlockReader reader(block);
  auto it = reader.NewIterator();

  it.Seek(entries[7].first);  // exact
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), entries[7].first);

  it.Seek("key00000003");  // between entries 1 ("...02") and 2 ("...04")
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), entries[2].first);

  it.Seek("");  // before everything
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), entries[0].first);

  it.Seek("zzz");  // after everything
  EXPECT_FALSE(it.Valid());
  EXPECT_TRUE(it.status().ok());
}

TEST(BlockReader, SeekThenNextContinuesInOrder) {
  const Entries entries = SortedEntries(40);
  const std::string block = Build(entries, 4);
  BlockReader reader(block);
  auto it = reader.NewIterator();
  it.Seek(entries[10].first);
  for (size_t i = 10; i < entries.size(); ++i) {
    ASSERT_TRUE(it.Valid());
    EXPECT_EQ(it.key(), entries[i].first);
    it.Next();
  }
  EXPECT_FALSE(it.Valid());
}

TEST(BlockReader, EveryKeyInTenThousandEntryBlockIsFound) {
  const Entries entries = SortedEntries(10000);
  const std::string block =
      Build(entries, BlockBuilder::kDefaultRestartInterval);
  BlockReader reader(block);
  ASSERT_TRUE(reader.status().ok());
  auto it = reader.NewIterator();
  for (const auto& [k, v] : entries) {
    it.Seek(k);
    ASSERT_TRUE(it.Valid()) << k;
    ASSERT_EQ(it.key(), k);
    ASSERT_EQ(it.value(), v);
  }
}

TEST(BlockReader, RandomSeeksMatchSortedVectorOracle) {
  std::mt19937 rng(42);
  for (int round = 0; round < 20; ++round) {
    std::map<std::string, std::string> sorted;
    const int n = 1 + static_cast<int>(rng() % 300);
    for (int i = 0; i < n; ++i) {
      std::string key(1 + rng() % 6, 'a');
      for (char& c : key) {
        c = static_cast<char>(
            'a' + rng() % 4);  // small alphabet: many shared prefixes
      }
      sorted[key] = std::to_string(rng());
    }
    const Entries entries(sorted.begin(), sorted.end());
    const int interval = 1 + static_cast<int>(rng() % 20);
    const std::string block = Build(entries, interval);
    BlockReader reader(block);
    ASSERT_TRUE(reader.status().ok());
    auto it = reader.NewIterator();
    for (int q = 0; q < 200; ++q) {
      std::string target(rng() % 8, 'a');
      for (char& c : target) {
        c = static_cast<char>('a' + rng() % 5);
      }
      const auto expected = sorted.lower_bound(target);
      it.Seek(target);
      if (expected == sorted.end()) {
        ASSERT_FALSE(it.Valid());
      } else {
        ASSERT_TRUE(it.Valid());
        ASSERT_EQ(it.key(), expected->first);
        ASSERT_EQ(it.value(), expected->second);
      }
    }
  }
}

TEST(BlockReader, EmptyValuesAndKeysRoundTrip) {
  const Entries entries = {{"", ""}, {"a", ""}, {"b", "x"}};
  const std::string block = Build(entries, 2);
  BlockReader reader(block);
  auto it = reader.NewIterator();
  size_t i = 0;
  for (it.SeekToFirst(); it.Valid(); it.Next(), ++i) {
    EXPECT_EQ(it.key(), entries[i].first);
    EXPECT_EQ(it.value(), entries[i].second);
  }
  EXPECT_EQ(i, entries.size());
}

TEST(BlockReader, CustomComparatorIsUsed) {
  // Reverse bytewise order: the builder gets keys in descending order.
  const auto reverse = [](std::string_view a, std::string_view b) {
    return b.compare(a);
  };
  Entries entries = SortedEntries(30);
  std::reverse(entries.begin(), entries.end());
  const std::string block = Build(entries, 4);
  BlockReader reader(block, reverse);
  auto it = reader.NewIterator();
  it.Seek(entries[5].first);
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), entries[5].first);
}

TEST(BlockReader, FlippedByteIsCorruption) {
  const std::string good = Build(SortedEntries(20), 4);
  for (size_t pos = 0; pos < good.size(); ++pos) {
    std::string bad = good;
    bad[pos] = static_cast<char>(bad[pos] ^ 0x01);
    BlockReader reader(bad);
    ASSERT_TRUE(reader.status().IsCorruption()) << "byte " << pos;
    auto it = reader.NewIterator();
    it.SeekToFirst();
    EXPECT_FALSE(it.Valid());
    it.Seek("key");
    EXPECT_FALSE(it.Valid());
    EXPECT_TRUE(it.status().IsCorruption());
  }
}

TEST(BlockReader, TruncatedBlockIsCorruption) {
  const std::string good = Build(SortedEntries(20), 4);
  for (size_t len = 0; len < good.size(); ++len) {
    BlockReader reader(std::string_view(good).substr(0, len));
    EXPECT_TRUE(reader.status().IsCorruption()) << "length " << len;
  }
}

}  // namespace
}  // namespace sediment
