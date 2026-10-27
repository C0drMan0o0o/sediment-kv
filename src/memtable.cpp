#include "sediment/memtable.h"

#include <cassert>
#include <cstring>

#include "sediment/coding.h"

namespace sediment {

namespace {

// Decode the varint32-length-prefixed bytes at `p`. Entries are written by
// MemTable::Add and lookup keys by Get, so the prefix is always well formed.
std::string_view GetLengthPrefixed(const char* p, const char** end) {
  uint32_t len = 0;
  std::string_view in(p, kMaxVarint32Bytes);
  Status s = GetVarint32(&in, &len);
  assert(s.ok());
  (void)s;
  const char* data = in.data();
  *end = data + len;
  return {data, len};
}

}  // namespace

int MemTable::EntryComparator::operator()(const char* a, const char* b) const {
  const char* end = nullptr;
  return CompareInternalKeys(GetLengthPrefixed(a, &end),
                             GetLengthPrefixed(b, &end));
}

MemTable::MemTable() : table_(EntryComparator(), &arena_) {}

void MemTable::Add(SequenceNumber sequence, ValueType type,
                   std::string_view key, std::string_view value) {
  const size_t internal_len = key.size() + 8;
  const size_t total =
      static_cast<size_t>(VarintLength(internal_len)) + internal_len +
      static_cast<size_t>(VarintLength(value.size())) + value.size();
  char* buf = arena_.Allocate(total);

  std::string encoded;
  AppendInternalKey(&encoded,
                    {.user_key = key, .sequence = sequence, .type = type});
  char* p = EncodeVarint32(buf, static_cast<uint32_t>(internal_len));
  std::memcpy(p, encoded.data(), internal_len);
  p += internal_len;
  p = EncodeVarint32(p, static_cast<uint32_t>(value.size()));
  if (!value.empty()) {
    std::memcpy(p, value.data(), value.size());
  }
  p += value.size();
  assert(p == buf + total);

  table_.Insert(buf);
}

MemTable::GetResult MemTable::Get(std::string_view key, SequenceNumber sequence,
                                  std::string* value) const {
  // kValue has the highest type number, so this probe sorts before every
  // version of `key` with sequence <= `sequence`.
  std::string probe;
  PutVarint32(&probe, static_cast<uint32_t>(key.size() + 8));
  AppendInternalKey(
      &probe,
      {.user_key = key, .sequence = sequence, .type = ValueType::kValue});

  Table::Iterator it(&table_);
  it.Seek(probe.data());
  if (!it.Valid()) {
    return GetResult::kNotFound;
  }

  const char* entry = it.key();
  const char* key_end = nullptr;
  const std::string_view internal_key = GetLengthPrefixed(entry, &key_end);
  ParsedInternalKey parsed;
  const Status s = ParseInternalKey(internal_key, &parsed);
  assert(s.ok());
  (void)s;
  if (parsed.user_key != key) {
    return GetResult::kNotFound;
  }
  if (parsed.type == ValueType::kDeletion) {
    return GetResult::kDeleted;
  }
  const char* value_end = nullptr;
  const std::string_view v = GetLengthPrefixed(key_end, &value_end);
  value->assign(v.data(), v.size());
  return GetResult::kFound;
}

std::string_view MemTable::Iterator::key() const {
  const char* key_end = nullptr;
  return GetLengthPrefixed(it_.key(), &key_end);
}

std::string_view MemTable::Iterator::value() const {
  const char* key_end = nullptr;
  GetLengthPrefixed(it_.key(), &key_end);
  const char* value_end = nullptr;
  return GetLengthPrefixed(key_end, &value_end);
}

}  // namespace sediment
