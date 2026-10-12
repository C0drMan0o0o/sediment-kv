#include "sediment/coding.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace sediment {
namespace {

TEST(Coding, Fixed32IsLittleEndian) {
  std::string s;
  PutFixed32(&s, 0x04030201);
  EXPECT_EQ(s, std::string("\x01\x02\x03\x04", 4));
}

TEST(Coding, Fixed64IsLittleEndian) {
  std::string s;
  PutFixed64(&s, 0x0807060504030201ULL);
  EXPECT_EQ(s, std::string("\x01\x02\x03\x04\x05\x06\x07\x08", 8));
}

TEST(Coding, Fixed32RoundTripsConsecutiveValues) {
  std::string s;
  for (uint32_t v = 0; v < 100000; ++v) {
    PutFixed32(&s, v);
  }
  std::string_view in = s;
  for (uint32_t v = 0; v < 100000; ++v) {
    uint32_t got = 0;
    ASSERT_TRUE(GetFixed32(&in, &got).ok());
    ASSERT_EQ(got, v);
  }
  EXPECT_TRUE(in.empty());
}

TEST(Coding, Fixed64RoundTripsBoundaryValues) {
  std::vector<uint64_t> values = {0,
                                  1,
                                  0xFF,
                                  0x100,
                                  0xFFFFFFFFULL,
                                  0x100000000ULL,
                                  std::numeric_limits<uint64_t>::max()};
  for (unsigned bit = 0; bit < 64; ++bit) {
    values.push_back(uint64_t{1} << bit);
    values.push_back((uint64_t{1} << bit) - 1);
  }
  std::string s;
  for (uint64_t v : values) {
    PutFixed64(&s, v);
  }
  std::string_view in = s;
  for (uint64_t v : values) {
    uint64_t got = 0;
    ASSERT_TRUE(GetFixed64(&in, &got).ok());
    ASSERT_EQ(got, v);
  }
  EXPECT_TRUE(in.empty());
}

TEST(Coding, FixedTruncatedInputIsCorruptionAndUnchanged) {
  const std::string s = "\x01\x02\x03";
  std::string_view in = s;
  uint32_t v32 = 7;
  EXPECT_TRUE(GetFixed32(&in, &v32).IsCorruption());
  EXPECT_EQ(v32, 7u);
  EXPECT_EQ(in.size(), 3u);

  const std::string t = "1234567";
  in = t;
  uint64_t v64 = 7;
  EXPECT_TRUE(GetFixed64(&in, &v64).IsCorruption());
  EXPECT_EQ(v64, 7u);
  EXPECT_EQ(in.size(), 7u);

  in = std::string_view();
  EXPECT_TRUE(GetFixed32(&in, &v32).IsCorruption());
}

TEST(Coding, Varint32KnownEncodings) {
  struct Case {
    uint32_t value;
    std::string bytes;
  };
  const std::vector<Case> cases = {
      {.value = 0, .bytes = std::string("\x00", 1)},
      {.value = 1, .bytes = "\x01"},
      {.value = 127, .bytes = "\x7F"},
      {.value = 128, .bytes = "\x80\x01"},
      {.value = 300, .bytes = "\xAC\x02"},
      {.value = 16383, .bytes = "\xFF\x7F"},
      {.value = 16384, .bytes = "\x80\x80\x01"},
      {.value = std::numeric_limits<uint32_t>::max(),
       .bytes = "\xFF\xFF\xFF\xFF\x0F"},
  };
  for (const auto& c : cases) {
    std::string s;
    PutVarint32(&s, c.value);
    EXPECT_EQ(s, c.bytes) << c.value;
    EXPECT_EQ(VarintLength(c.value), static_cast<int>(c.bytes.size()));
  }
}

TEST(Coding, Varint32RoundTripsAllBitBoundaries) {
  std::vector<uint32_t> values = {0, std::numeric_limits<uint32_t>::max()};
  for (unsigned bit = 0; bit < 32; ++bit) {
    values.push_back(uint32_t{1} << bit);
    values.push_back((uint32_t{1} << bit) - 1);
    values.push_back((uint32_t{1} << bit) + 1);
  }
  std::string s;
  for (uint32_t v : values) {
    PutVarint32(&s, v);
  }
  std::string_view in = s;
  for (uint32_t v : values) {
    uint32_t got = 0;
    ASSERT_TRUE(GetVarint32(&in, &got).ok());
    ASSERT_EQ(got, v);
  }
  EXPECT_TRUE(in.empty());
}

TEST(Coding, Varint64RoundTripsAllBitBoundaries) {
  std::vector<uint64_t> values = {0, std::numeric_limits<uint64_t>::max()};
  for (unsigned bit = 0; bit < 64; ++bit) {
    values.push_back(uint64_t{1} << bit);
    values.push_back((uint64_t{1} << bit) - 1);
    values.push_back((uint64_t{1} << bit) + 1);
  }
  std::string s;
  for (uint64_t v : values) {
    PutVarint64(&s, v);
  }
  std::string_view in = s;
  for (uint64_t v : values) {
    uint64_t got = 0;
    ASSERT_TRUE(GetVarint64(&in, &got).ok());
    ASSERT_EQ(got, v);
  }
  EXPECT_TRUE(in.empty());
}

TEST(Coding, Varint64MaxIsTenBytes) {
  std::string s;
  PutVarint64(&s, std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(s.size(), kMaxVarint64Bytes);
  EXPECT_EQ(VarintLength(std::numeric_limits<uint64_t>::max()), 10);
}

TEST(Coding, VarintEveryTruncatedPrefixIsCorruption) {
  std::string s;
  PutVarint64(&s, std::numeric_limits<uint64_t>::max());
  for (size_t len = 0; len < s.size(); ++len) {
    std::string_view in(s.data(), len);
    uint64_t v = 7;
    EXPECT_TRUE(GetVarint64(&in, &v).IsCorruption()) << len;
    EXPECT_EQ(v, 7u);
    EXPECT_EQ(in.size(), len);
  }
  std::string s32;
  PutVarint32(&s32, std::numeric_limits<uint32_t>::max());
  for (size_t len = 0; len < s32.size(); ++len) {
    std::string_view in(s32.data(), len);
    uint32_t v = 7;
    EXPECT_TRUE(GetVarint32(&in, &v).IsCorruption()) << len;
  }
}

TEST(Coding, Varint32RejectsOverflowAndOverlong) {
  uint32_t v = 0;
  // Fifth byte carries bits above 2^32.
  std::string overflow("\xFF\xFF\xFF\xFF\x1F", 5);
  std::string_view in = overflow;
  EXPECT_TRUE(GetVarint32(&in, &v).IsCorruption());
  // Continuation bit set on every one of the 5 permitted bytes.
  std::string overlong("\x80\x80\x80\x80\x80\x01", 6);
  in = overlong;
  EXPECT_TRUE(GetVarint32(&in, &v).IsCorruption());
}

TEST(Coding, Varint64RejectsOverflowAndOverlong) {
  uint64_t v = 0;
  // Tenth byte may only be 0 or 1.
  std::string overflow = std::string(9, '\xFF') + std::string("\x02", 1);
  std::string_view in = overflow;
  EXPECT_TRUE(GetVarint64(&in, &v).IsCorruption());
  std::string overlong = std::string(10, '\x80') + std::string("\x01", 1);
  in = overlong;
  EXPECT_TRUE(GetVarint64(&in, &v).IsCorruption());
}

TEST(Coding, VarintDecodeLeavesTrailingBytes) {
  std::string s;
  PutVarint32(&s, 300);
  s += "tail";
  std::string_view in = s;
  uint32_t v = 0;
  ASSERT_TRUE(GetVarint32(&in, &v).ok());
  EXPECT_EQ(v, 300u);
  EXPECT_EQ(in, "tail");
}

}  // namespace
}  // namespace sediment
