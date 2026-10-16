#include "sediment/internal_key.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace sediment {
namespace {

std::string Encode(std::string_view user_key, SequenceNumber seq,
                   ValueType type) {
  std::string out;
  AppendInternalKey(&out, {user_key, seq, type});
  return out;
}

TEST(InternalKey, RoundTrip) {
  const std::string_view keys[] = {"", "a", "hello",
                                   std::string_view("a\0b", 3)};
  const SequenceNumber seqs[] = {0, 1, 255, 256, kMaxSequenceNumber};
  for (auto key : keys) {
    for (auto seq : seqs) {
      for (auto type : {ValueType::kDeletion, ValueType::kValue}) {
        std::string enc = Encode(key, seq, type);
        EXPECT_EQ(enc.size(), key.size() + 8);
        ParsedInternalKey parsed;
        ASSERT_TRUE(ParseInternalKey(enc, &parsed).ok());
        EXPECT_EQ(parsed.user_key, key);
        EXPECT_EQ(parsed.sequence, seq);
        EXPECT_EQ(parsed.type, type);
        EXPECT_EQ(ExtractUserKey(enc), key);
      }
    }
  }
}

TEST(InternalKey, ParseRejectsShortInput) {
  ParsedInternalKey parsed;
  EXPECT_TRUE(ParseInternalKey("1234567", &parsed).IsCorruption());
  EXPECT_TRUE(ParseInternalKey("", &parsed).IsCorruption());
}

TEST(InternalKey, ParseRejectsUnknownType) {
  std::string enc = Encode("k", 7, ValueType::kValue);
  enc[1] = 0x7f;  // low byte of the tag is the type
  ParsedInternalKey parsed;
  EXPECT_TRUE(ParseInternalKey(enc, &parsed).IsCorruption());
}

TEST(InternalKey, NewerVersionsSortFirst) {
  EXPECT_LT(CompareInternalKeys(Encode("k", 10, ValueType::kValue),
                                Encode("k", 5, ValueType::kValue)),
            0);
  EXPECT_GT(CompareInternalKeys(Encode("k", 5, ValueType::kValue),
                                Encode("k", 10, ValueType::kValue)),
            0);
  EXPECT_EQ(CompareInternalKeys(Encode("k", 5, ValueType::kValue),
                                Encode("k", 5, ValueType::kValue)),
            0);
}

TEST(InternalKey, SequenceBeyondOneByteOrdersCorrectly) {
  // Guards against comparing the little-endian tag bytewise.
  EXPECT_LT(CompareInternalKeys(Encode("k", 256, ValueType::kValue),
                                Encode("k", 255, ValueType::kValue)),
            0);
}

TEST(InternalKey, UserKeyDominatesSequence) {
  EXPECT_LT(CompareInternalKeys(Encode("a", 1, ValueType::kValue),
                                Encode("b", 100, ValueType::kValue)),
            0);
  // A prefix user key sorts before its extensions regardless of sequence.
  EXPECT_LT(CompareInternalKeys(Encode("ab", 1, ValueType::kValue),
                                Encode("abc", 100, ValueType::kValue)),
            0);
  EXPECT_GT(CompareInternalKeys(Encode("abc", 100, ValueType::kValue),
                                Encode("ab", 1, ValueType::kValue)),
            0);
}

TEST(InternalKey, DeletionSortsAfterPutAtSameSequence) {
  EXPECT_GT(CompareInternalKeys(Encode("k", 5, ValueType::kDeletion),
                                Encode("k", 5, ValueType::kValue)),
            0);
}

TEST(InternalKey, SortedOrderGroupsKeysNewestFirst) {
  std::vector<std::string> keys = {
      Encode("b", 1, ValueType::kValue),    Encode("a", 3, ValueType::kValue),
      Encode("b", 9, ValueType::kDeletion), Encode("a", 7, ValueType::kValue),
      Encode("a", 1, ValueType::kValue),
  };
  std::sort(keys.begin(), keys.end(),
            [](const std::string& a, const std::string& b) {
              return InternalKeyComparator()(a, b) < 0;
            });
  std::vector<std::pair<std::string, SequenceNumber>> got;
  for (const auto& k : keys) {
    ParsedInternalKey p;
    ASSERT_TRUE(ParseInternalKey(k, &p).ok());
    got.emplace_back(std::string(p.user_key), p.sequence);
  }
  const std::vector<std::pair<std::string, SequenceNumber>> want = {
      {"a", 7}, {"a", 3}, {"a", 1}, {"b", 9}, {"b", 1}};
  EXPECT_EQ(got, want);
}

}  // namespace
}  // namespace sediment
