#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "sediment/status.h"

namespace sediment {

using SequenceNumber = uint64_t;

// Sequence numbers occupy the upper 56 bits of the packed tag.
inline constexpr uint64_t kTagTypeBits = 8;
inline constexpr SequenceNumber kMaxSequenceNumber =
    (uint64_t{1} << (64 - kTagTypeBits)) - 1;

// The numeric values are part of the on-disk format; do not reorder.
// Tags sort descending, so at the same sequence number a put (1) sorts before
// a delete (0).
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

// The user-key portion of an encoded internal key. Input shorter than the
// 8-byte tag is malformed; it is returned whole, as if it had tag 0, so that
// nothing reads outside it.
std::string_view ExtractUserKey(std::string_view internal_key);

// Orders by user key ascending (bytewise), then sequence descending, so newer
// versions of a key sort first. Returns <0, 0, >0 like memcmp.
//
// Safe on any bytes, including keys from a corrupt file or a bare user key
// passed by mistake: input shorter than the tag compares as that user key with
// tag 0 (sequence 0, kDeletion). It never reads outside its arguments, but it
// cannot report the malformed key; ParseInternalKey does that.
int CompareInternalKeys(std::string_view a, std::string_view b);

// Function object form, for use with SkipList.
struct InternalKeyComparator {
  int operator()(std::string_view a, std::string_view b) const {
    return CompareInternalKeys(a, b);
  }
};

}  // namespace sediment
