#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "sediment/status.h"

namespace sediment {

using SequenceNumber = uint64_t;

// Sequence numbers occupy the upper 56 bits of the packed tag.
inline constexpr SequenceNumber kMaxSequenceNumber = (uint64_t{1} << 56) - 1;

// The numeric values are part of the on-disk format; do not reorder.
// A delete sorts before a put at the same sequence number.
enum class ValueType : uint8_t {
  kDeletion = 0,
  kValue = 1,
};

struct ParsedInternalKey {
  std::string_view user_key;
  SequenceNumber sequence = 0;
  ValueType type = ValueType::kValue;
};

// Internal key layout: user_key bytes, then fixed64 little-endian of
// (sequence << 8 | type). `sequence` must be <= kMaxSequenceNumber.
void AppendInternalKey(std::string* dst, const ParsedInternalKey& key);

// Parse an encoded internal key. Returns Corruption if it is shorter than the
// 8-byte tag or carries an unknown type. The result's user_key views `input`.
Status ParseInternalKey(std::string_view input, ParsedInternalKey* result);

// The user-key portion of an encoded internal key (which must be >= 8 bytes).
std::string_view ExtractUserKey(std::string_view internal_key);

// Orders by user key ascending (bytewise), then sequence descending, so newer
// versions of a key sort first. Returns <0, 0, >0 like memcmp.
int CompareInternalKeys(std::string_view a, std::string_view b);

// Function object form, for use with SkipList.
struct InternalKeyComparator {
  int operator()(std::string_view a, std::string_view b) const {
    return CompareInternalKeys(a, b);
  }
};

}  // namespace sediment
