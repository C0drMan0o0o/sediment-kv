#include "sediment/memtable.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace sediment {
namespace {

TEST(MemTable, MissingKeyIsNotFound) {
  MemTable mem;
  std::string value;
  EXPECT_EQ(mem.Get("k", 10, &value), MemTable::GetResult::kNotFound);
  mem.Add(1, ValueType::kValue, "a", "1");
  EXPECT_EQ(mem.Get("b", 10, &value), MemTable::GetResult::kNotFound);
  EXPECT_EQ(mem.Get("", 10, &value), MemTable::GetResult::kNotFound);
}

TEST(MemTable, PutThenGet) {
  MemTable mem;
  mem.Add(1, ValueType::kValue, "k", "v1");
  std::string value;
  ASSERT_EQ(mem.Get("k", 1, &value), MemTable::GetResult::kFound);
  EXPECT_EQ(value, "v1");
}

TEST(MemTable, NewerValueShadowsOlder) {
  MemTable mem;
  mem.Add(1, ValueType::kValue, "k", "old");
  mem.Add(2, ValueType::kValue, "k", "new");
  std::string value;
  ASSERT_EQ(mem.Get("k", 2, &value), MemTable::GetResult::kFound);
  EXPECT_EQ(value, "new");
}

TEST(MemTable, SequenceBoundsVisibility) {
  MemTable mem;
  mem.Add(5, ValueType::kValue, "k", "v5");
  mem.Add(9, ValueType::kValue, "k", "v9");
  std::string value;
  EXPECT_EQ(mem.Get("k", 4, &value), MemTable::GetResult::kNotFound);
  ASSERT_EQ(mem.Get("k", 5, &value), MemTable::GetResult::kFound);
  EXPECT_EQ(value, "v5");
  ASSERT_EQ(mem.Get("k", 8, &value), MemTable::GetResult::kFound);
  EXPECT_EQ(value, "v5");
  ASSERT_EQ(mem.Get("k", 9, &value), MemTable::GetResult::kFound);
  EXPECT_EQ(value, "v9");
  ASSERT_EQ(mem.Get("k", kMaxSequenceNumber, &value),
            MemTable::GetResult::kFound);
  EXPECT_EQ(value, "v9");
}

TEST(MemTable, TombstoneHidesOlderValue) {
  MemTable mem;
  mem.Add(1, ValueType::kValue, "k", "v");
  mem.Add(2, ValueType::kDeletion, "k", "");
  std::string value = "untouched";
  EXPECT_EQ(mem.Get("k", 2, &value), MemTable::GetResult::kDeleted);
  EXPECT_EQ(value, "untouched");
  // A reader at an earlier sequence still sees the value.
  ASSERT_EQ(mem.Get("k", 1, &value), MemTable::GetResult::kFound);
  EXPECT_EQ(value, "v");
}

TEST(MemTable, ReinsertAfterDelete) {
  MemTable mem;
  mem.Add(1, ValueType::kValue, "k", "a");
  mem.Add(2, ValueType::kDeletion, "k", "");
  mem.Add(3, ValueType::kValue, "k", "b");
  std::string value;
  ASSERT_EQ(mem.Get("k", 3, &value), MemTable::GetResult::kFound);
  EXPECT_EQ(value, "b");
  EXPECT_EQ(mem.Get("k", 2, &value), MemTable::GetResult::kDeleted);
  ASSERT_EQ(mem.Get("k", 1, &value), MemTable::GetResult::kFound);
  EXPECT_EQ(value, "a");
}

TEST(MemTable, TombstoneDoesNotAffectNeighbours) {
  MemTable mem;
  mem.Add(1, ValueType::kValue, "a", "1");
  mem.Add(2, ValueType::kValue, "c", "3");
  mem.Add(3, ValueType::kDeletion, "b", "");
  std::string value;
  EXPECT_EQ(mem.Get("b", 3, &value), MemTable::GetResult::kDeleted);
  ASSERT_EQ(mem.Get("a", 3, &value), MemTable::GetResult::kFound);
  EXPECT_EQ(value, "1");
  ASSERT_EQ(mem.Get("c", 3, &value), MemTable::GetResult::kFound);
  EXPECT_EQ(value, "3");
}

TEST(MemTable, BinarySafeKeysAndValues) {
  MemTable mem;
  const std::string key("a\0b", 3);
  const std::string val("x\0y\0", 4);
  mem.Add(1, ValueType::kValue, key, val);
  mem.Add(2, ValueType::kValue, "", "");
  std::string value;
  ASSERT_EQ(mem.Get(key, 1, &value), MemTable::GetResult::kFound);
  EXPECT_EQ(value, val);
  EXPECT_EQ(mem.Get("a", 1, &value), MemTable::GetResult::kNotFound);
  ASSERT_EQ(mem.Get("", 2, &value), MemTable::GetResult::kFound);
  EXPECT_EQ(value, "");
}

TEST(MemTable, PrefixKeyIsDistinct) {
  MemTable mem;
  mem.Add(1, ValueType::kValue, "ab", "long");
  std::string value;
  EXPECT_EQ(mem.Get("a", 1, &value), MemTable::GetResult::kNotFound);
  EXPECT_EQ(mem.Get("abc", 1, &value), MemTable::GetResult::kNotFound);
}

TEST(MemTable, ApproximateMemoryGrows) {
  MemTable mem;
  const size_t empty = mem.ApproximateMemoryUsage();
  // The arena grows in blocks, so a single small entry may not change usage.
  for (SequenceNumber i = 2; i < 2000; ++i) {
    mem.Add(i, ValueType::kValue, "key" + std::to_string(i),
            std::string(100, 'x'));
  }
  // Usage must cover the payload: 2000 entries of >100 bytes each.
  EXPECT_GT(mem.ApproximateMemoryUsage(), empty + size_t{1999} * 100);
}

TEST(MemTable, IteratorYieldsInternalKeyOrder) {
  MemTable mem;
  MemTable::Iterator empty(&mem);
  empty.SeekToFirst();
  EXPECT_FALSE(empty.Valid());

  mem.Add(1, ValueType::kValue, "b", "old");
  mem.Add(2, ValueType::kValue, "a", "x");
  mem.Add(3, ValueType::kValue, "b", "new");

  MemTable::Iterator it(&mem);
  std::vector<std::pair<std::string, std::string>> got;
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    ParsedInternalKey parsed;
    ASSERT_TRUE(ParseInternalKey(it.key(), &parsed).ok());
    got.emplace_back(
        std::string(parsed.user_key) + "@" + std::to_string(parsed.sequence),
        std::string(it.value()));
  }
  const std::vector<std::pair<std::string, std::string>> expected = {
      {"a@2", "x"}, {"b@3", "new"}, {"b@1", "old"}};
  EXPECT_EQ(got, expected);
}

}  // namespace
}  // namespace sediment
