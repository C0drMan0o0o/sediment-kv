#include "sediment/internal_key.h"

#include <cassert>
#include <cstring>

#include "sediment/coding.h"

namespace sediment {

namespace {

constexpr size_t kTagBytes = 8;

uint64_t PackTag(SequenceNumber sequence, ValueType type) {
  assert(sequence <= kMaxSequenceNumber);
  return (sequence << 8) | static_cast<uint8_t>(type);
}

uint64_t DecodeTag(std::string_view internal_key) {
  uint64_t tag = 0;
  const char* p = internal_key.data() + internal_key.size() - kTagBytes;
  for (size_t i = 0; i < kTagBytes; ++i) {
    tag |= static_cast<uint64_t>(static_cast<uint8_t>(p[i])) << (8 * i);
  }
  return tag;
}

}  // namespace

void AppendInternalKey(std::string* dst, const ParsedInternalKey& key) {
  dst->append(key.user_key.data(), key.user_key.size());
  PutFixed64(dst, PackTag(key.sequence, key.type));
}

Status ParseInternalKey(std::string_view input, ParsedInternalKey* result) {
  if (input.size() < kTagBytes) {
    return Status::Corruption("internal key shorter than tag");
  }
  const uint64_t tag = DecodeTag(input);
  const uint8_t type = static_cast<uint8_t>(tag & 0xff);
  if (type > static_cast<uint8_t>(ValueType::kValue)) {
    return Status::Corruption("internal key has unknown value type");
  }
  result->user_key = input.substr(0, input.size() - kTagBytes);
  result->sequence = tag >> 8;
  result->type = static_cast<ValueType>(type);
  return Status::OK();
}

std::string_view ExtractUserKey(std::string_view internal_key) {
  assert(internal_key.size() >= kTagBytes);
  return internal_key.substr(0, internal_key.size() - kTagBytes);
}

int CompareInternalKeys(std::string_view a, std::string_view b) {
  const int r = ExtractUserKey(a).compare(ExtractUserKey(b));
  if (r != 0) return r < 0 ? -1 : 1;
  const uint64_t ta = DecodeTag(a);
  const uint64_t tb = DecodeTag(b);
  if (ta > tb) return -1;  // higher sequence (newer) sorts first
  if (ta < tb) return 1;
  return 0;
}

}  // namespace sediment
