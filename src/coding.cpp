#include "sediment/coding.h"

#include <array>

namespace sediment {

namespace {

constexpr uint8_t kVarintContinue = 0x80;
constexpr uint8_t kVarintPayloadMask = 0x7F;
constexpr unsigned kVarintPayloadBits = 7;

uint8_t ByteAt(std::string_view s, size_t i) {
  return static_cast<uint8_t>(s[i]);
}

// Shared varint decoder; `max_bytes` is 5 or 10 and `Int` the target width.
template <typename Int>
Status GetVarint(std::string_view* input, Int* value, size_t max_bytes) {
  Int result = 0;
  for (size_t i = 0; i < input->size() && i < max_bytes; ++i) {
    const uint8_t byte = ByteAt(*input, i);
    const auto shift = static_cast<unsigned>(kVarintPayloadBits * i);
    const Int payload = byte & kVarintPayloadMask;
    // On the last permitted byte, payload bits must not overflow Int.
    if (i + 1 == max_bytes && (payload >> (sizeof(Int) * 8 - shift)) != 0) {
      return Status::Corruption("varint overflow");
    }
    result |= payload << shift;
    if ((byte & kVarintContinue) == 0) {
      *value = result;
      input->remove_prefix(i + 1);
      return Status::OK();
    }
  }
  if (input->size() < max_bytes) {
    return Status::Corruption("truncated varint");
  }
  return Status::Corruption("varint too long");
}

}  // namespace

void EncodeFixed32(char* dst, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) {
    dst[i] = static_cast<char>(value >> (8 * i));
  }
}

void EncodeFixed64(char* dst, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) {
    dst[i] = static_cast<char>(value >> (8 * i));
  }
}

char* EncodeVarint64(char* dst, uint64_t value) {
  while (value >= kVarintContinue) {
    *dst++ = static_cast<char>((value & kVarintPayloadMask) | kVarintContinue);
    value >>= kVarintPayloadBits;
  }
  *dst++ = static_cast<char>(value);
  return dst;
}

char* EncodeVarint32(char* dst, uint32_t value) {
  return EncodeVarint64(dst, value);
}

void PutFixed32(std::string* dst, uint32_t value) {
  std::array<char, 4> buf;
  EncodeFixed32(buf.data(), value);
  dst->append(buf.data(), buf.size());
}

void PutFixed64(std::string* dst, uint64_t value) {
  std::array<char, 8> buf;
  EncodeFixed64(buf.data(), value);
  dst->append(buf.data(), buf.size());
}

void PutVarint32(std::string* dst, uint32_t value) {
  std::array<char, kMaxVarint32Bytes> buf;
  dst->append(buf.data(), EncodeVarint32(buf.data(), value) - buf.data());
}

void PutVarint64(std::string* dst, uint64_t value) {
  std::array<char, kMaxVarint64Bytes> buf;
  dst->append(buf.data(), EncodeVarint64(buf.data(), value) - buf.data());
}

int VarintLength(uint64_t value) {
  int len = 1;
  while (value >= kVarintContinue) {
    value >>= kVarintPayloadBits;
    ++len;
  }
  return len;
}

Status GetFixed32(std::string_view* input, uint32_t* value) {
  if (input->size() < 4) {
    return Status::Corruption("truncated fixed32");
  }
  uint32_t result = 0;
  for (unsigned i = 0; i < 4; ++i) {
    result |= static_cast<uint32_t>(ByteAt(*input, i)) << (8 * i);
  }
  *value = result;
  input->remove_prefix(4);
  return Status::OK();
}

Status GetFixed64(std::string_view* input, uint64_t* value) {
  if (input->size() < 8) {
    return Status::Corruption("truncated fixed64");
  }
  uint64_t result = 0;
  for (unsigned i = 0; i < 8; ++i) {
    result |= static_cast<uint64_t>(ByteAt(*input, i)) << (8 * i);
  }
  *value = result;
  input->remove_prefix(8);
  return Status::OK();
}

Status GetVarint32(std::string_view* input, uint32_t* value) {
  return GetVarint(input, value, kMaxVarint32Bytes);
}

Status GetVarint64(std::string_view* input, uint64_t* value) {
  return GetVarint(input, value, kMaxVarint64Bytes);
}

}  // namespace sediment
