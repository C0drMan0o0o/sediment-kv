#include "sediment/block_builder.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "sediment/coding.h"
#include "sediment/crc32c.h"

namespace sediment {
namespace {

// Expected trailer: restart offsets, their count, then the masked CRC of all
// bytes before it.
std::string WithTrailer(std::string body,
                        std::initializer_list<uint32_t> restarts) {
  for (const uint32_t r : restarts) {
    PutFixed32(&body, r);
  }
  PutFixed32(&body, static_cast<uint32_t>(restarts.size()));
  PutFixed32(&body, crc32c::Mask(crc32c::Value(body.data(), body.size())));
  return body;
}

TEST(BlockBuilder, EmptyBlockHasOnlyTrailer) {
  BlockBuilder builder;
  EXPECT_TRUE(builder.empty());
  EXPECT_EQ(builder.CurrentSizeEstimate(), 12U);
  const std::string_view block = builder.Finish();
  EXPECT_EQ(std::string(block), WithTrailer("", {0}));
}

TEST(BlockBuilder, SharesPrefixWithPreviousKey) {
  BlockBuilder builder;
  builder.Add("apple", "1");
  builder.Add("applesauce", "22");
  builder.Add("apply", "3");

  // {shared, non_shared, value_length} then key suffix then value.
  const std::string expected_body =
      std::string("\x00\x05\x01", 3) + "apple" + "1" +   // full key
      std::string("\x05\x05\x02", 3) + "sauce" + "22" +  // shares "apple"
      std::string("\x04\x01\x01", 3) + "y" + "3";        // shares "appl"
  EXPECT_EQ(std::string(builder.Finish()), WithTrailer(expected_body, {0}));
}

TEST(BlockBuilder, RestartsEveryIntervalEntries) {
  BlockBuilder builder(2);
  builder.Add("aa", "v");
  builder.Add("ab", "v");
  builder.Add("ac", "v");  // restart: stored in full despite sharing "a"
  builder.Add("ad", "v");

  const std::string expected_body =
      std::string("\x00\x02\x01", 3) + "aa" + "v" +  // 6 bytes, offset 0
      std::string("\x01\x01\x01", 3) + "b" + "v" +   // 5 bytes
      std::string("\x00\x02\x01", 3) + "ac" + "v" +  // restart at offset 11
      std::string("\x01\x01\x01", 3) + "d" + "v";
  EXPECT_EQ(std::string(builder.Finish()), WithTrailer(expected_body, {0, 11}));
}

TEST(BlockBuilder, HandlesEmptyKeysAndValues) {
  BlockBuilder builder;
  builder.Add("", "");
  builder.Add("k", "");
  const std::string expected_body =
      std::string("\x00\x00\x00", 3) + std::string("\x00\x01\x00", 3) + "k";
  EXPECT_EQ(std::string(builder.Finish()), WithTrailer(expected_body, {0}));
}

TEST(BlockBuilder, LongValueUsesMultiByteVarint) {
  BlockBuilder builder;
  const std::string value(300, 'x');
  builder.Add("k", value);
  std::string expected_body = std::string("\x00\x01", 2);
  PutVarint32(&expected_body, 300);
  expected_body += "k" + value;
  EXPECT_EQ(std::string(builder.Finish()), WithTrailer(expected_body, {0}));
}

TEST(BlockBuilder, SizeEstimateMatchesFinishedSize) {
  BlockBuilder builder(3);
  for (int i = 0; i < 20; ++i) {
    builder.Add("key" + std::to_string(100 + i), "value");
    const size_t estimate = builder.CurrentSizeEstimate();
    BlockBuilder copy(3);
    for (int j = 0; j <= i; ++j) {
      copy.Add("key" + std::to_string(100 + j), "value");
    }
    EXPECT_EQ(copy.Finish().size(), estimate);
  }
}

TEST(BlockBuilder, ResetAllowsReuseWithIdenticalOutput) {
  BlockBuilder builder;
  builder.Add("a", "1");
  builder.Add("b", "2");
  const std::string first(builder.Finish());

  builder.Reset();
  EXPECT_TRUE(builder.empty());
  builder.Add("a", "1");
  builder.Add("b", "2");
  EXPECT_EQ(std::string(builder.Finish()), first);
}

TEST(BlockBuilder, CrcDetectsFlippedBit) {
  BlockBuilder builder;
  builder.Add("key", "value");
  std::string block(builder.Finish());
  const size_t body = block.size() - 4;
  std::string_view tail(block.data() + body, 4);
  uint32_t stored = 0;
  ASSERT_TRUE(GetFixed32(&tail, &stored).ok());
  EXPECT_EQ(crc32c::Unmask(stored), crc32c::Value(block.data(), body));

  block[2] = static_cast<char>(static_cast<uint8_t>(block[2]) ^ 0x01U);
  EXPECT_NE(crc32c::Unmask(stored), crc32c::Value(block.data(), body));
}

}  // namespace
}  // namespace sediment
