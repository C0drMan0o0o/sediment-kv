#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "sediment/status.h"

namespace sediment {

// Fixed-width integers are little-endian regardless of host byte order.
inline constexpr size_t kMaxVarint32Bytes = 5;
inline constexpr size_t kMaxVarint64Bytes = 10;

// Encode into `dst`, which must have room for the full encoding
// (4 / 8 bytes for fixed, kMaxVarint*Bytes for varint).
void EncodeFixed32(char* dst, uint32_t value);
void EncodeFixed64(char* dst, uint64_t value);
// Return a pointer one past the last byte written.
char* EncodeVarint32(char* dst, uint32_t value);
char* EncodeVarint64(char* dst, uint64_t value);

// Append the encoding to `dst`.
void PutFixed32(std::string* dst, uint32_t value);
void PutFixed64(std::string* dst, uint64_t value);
void PutVarint32(std::string* dst, uint32_t value);
void PutVarint64(std::string* dst, uint64_t value);

// Number of bytes the varint encoding of `value` occupies.
int VarintLength(uint64_t value);

// Decode from the front of `*input`. On success, store the value, advance
// `*input` past the consumed bytes and return OK. On truncated or malformed
// input, return Corruption and leave `*input` and `*value` unchanged.
Status GetFixed32(std::string_view* input, uint32_t* value);
Status GetFixed64(std::string_view* input, uint64_t* value);
Status GetVarint32(std::string_view* input, uint32_t* value);
Status GetVarint64(std::string_view* input, uint64_t* value);

}  // namespace sediment
