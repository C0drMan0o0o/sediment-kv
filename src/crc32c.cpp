#include "sediment/crc32c.h"

#include <array>

namespace sediment::crc32c {
namespace {

constexpr uint32_t kReversedPolynomial = 0x82f63b78;

constexpr std::array<uint32_t, 256> MakeTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ ((crc & 1U) != 0 ? kReversedPolynomial : 0);
    }
    table[i] = crc;
  }
  return table;
}

constexpr std::array<uint32_t, 256> kTable = MakeTable();

}  // namespace

uint32_t Extend(uint32_t init_crc, const char* data, size_t n) {
  uint32_t crc = ~init_crc;
  for (size_t i = 0; i < n; ++i) {
    const auto byte = static_cast<uint8_t>(data[i]);
    crc = kTable[(crc ^ byte) & 0xffU] ^ (crc >> 8U);
  }
  return ~crc;
}

}  // namespace sediment::crc32c
