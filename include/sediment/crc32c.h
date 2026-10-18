#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>

namespace sediment::crc32c {

// CRC32C (Castagnoli, polynomial 0x1EDC6F41, reflected 0x82F63B78).

// Return the CRC32C of `data[0, n)` extended from `init_crc`, the CRC of some
// earlier bytes. Extend(Value(a), b) equals Value(a followed by b).
uint32_t Extend(uint32_t init_crc, const char* data, size_t n);

// Return the CRC32C of `data[0, n)`.
inline uint32_t Value(const char* data, size_t n) {
  return Extend(0, data, n);
}

// Stored CRCs are masked so that computing the CRC of a string that itself
// contains embedded CRCs does not produce degenerate results.
inline constexpr uint32_t kMaskDelta = 0xa282ead8;

// Return a masked representation of `crc`.
inline uint32_t Mask(uint32_t crc) {
  // Rotate right by 15 bits and add a constant.
  return std::rotr(crc, 15) + kMaskDelta;
}

// Return the CRC whose masked representation is `masked_crc`.
inline uint32_t Unmask(uint32_t masked_crc) {
  const uint32_t rot = masked_crc - kMaskDelta;
  return std::rotl(rot, 15);
}

}  // namespace sediment::crc32c
