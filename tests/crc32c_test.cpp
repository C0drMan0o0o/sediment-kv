#include "sediment/crc32c.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>

namespace sediment::crc32c {
namespace {

uint32_t ValueOf(const std::string& s) {
  return Value(s.data(), s.size());
}

// Known vectors from RFC 3720 (iSCSI), appendix B.4.
TEST(Crc32c, Rfc3720Zeros) {
  EXPECT_EQ(ValueOf(std::string(32, '\x00')), 0x8a9136aaU);
}

TEST(Crc32c, Rfc3720Ones) {
  EXPECT_EQ(ValueOf(std::string(32, '\xff')), 0x62a8ab43U);
}

TEST(Crc32c, Rfc3720Ascending) {
  std::string s;
  for (int i = 0; i < 32; ++i) {
    s.push_back(static_cast<char>(i));
  }
  EXPECT_EQ(ValueOf(s), 0x46dd794eU);
}

TEST(Crc32c, Rfc3720Descending) {
  std::string s;
  for (int i = 31; i >= 0; --i) {
    s.push_back(static_cast<char>(i));
  }
  EXPECT_EQ(ValueOf(s), 0x113fdb5cU);
}

TEST(Crc32c, Rfc3720ReadCommandPdu) {
  const std::array<unsigned char, 48> pdu = {
      0x01, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
      0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x18, 0x28, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  EXPECT_EQ(Value(reinterpret_cast<const char*>(pdu.data()), pdu.size()),
            0xd9963a56U);
}

TEST(Crc32c, CheckString) {
  EXPECT_EQ(ValueOf("123456789"), 0xe3069283U);
}

TEST(Crc32c, EmptyInputIsZero) {
  EXPECT_EQ(Value("", 0), 0U);
}

TEST(Crc32c, ValuesDifferForDifferentInputs) {
  EXPECT_NE(ValueOf("a"), ValueOf("foo"));
}

TEST(Crc32c, ExtendMatchesWholeBuffer) {
  const std::string hello = "hello ";
  const std::string world = "world";
  EXPECT_EQ(ValueOf("hello world"),
            Extend(ValueOf(hello), world.data(), world.size()));
}

TEST(Crc32c, ExtendAtEverySplitPoint) {
  const std::string s = "The quick brown fox jumps over the lazy dog";
  const uint32_t whole = ValueOf(s);
  for (size_t split = 0; split <= s.size(); ++split) {
    const uint32_t head = Value(s.data(), split);
    EXPECT_EQ(Extend(head, s.data() + split, s.size() - split), whole)
        << "split=" << split;
  }
}

TEST(Crc32c, MaskRoundTrips) {
  const uint32_t crc = ValueOf("foo");
  EXPECT_NE(Mask(crc), crc);
  EXPECT_NE(Mask(Mask(crc)), crc);
  EXPECT_EQ(Unmask(Mask(crc)), crc);
  EXPECT_EQ(Unmask(Unmask(Mask(Mask(crc)))), crc);
}

TEST(Crc32c, MaskOfZeroIsDelta) {
  EXPECT_EQ(Mask(0), 0xa282ead8U);
}

TEST(Crc32c, MaskRoundTripsBoundaryValues) {
  for (const uint32_t v : {0U, 1U, 0x80000000U, 0xffffffffU, kMaskDelta}) {
    EXPECT_EQ(Unmask(Mask(v)), v);
  }
}

}  // namespace
}  // namespace sediment::crc32c
