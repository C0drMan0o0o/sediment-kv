#include "sediment/recovery.h"

#include <memory>
#include <utility>

#include "sediment/coding.h"
#include "sediment/file.h"

namespace sediment {

namespace {

Status GetLengthPrefixed(std::string_view* input, std::string_view* result) {
  uint32_t len = 0;
  Status s = GetVarint32(input, &len);
  if (!s.ok()) {
    return s;
  }
  if (input->size() < len) {
    return Status::Corruption("truncated log entry");
  }
  *result = input->substr(0, len);
  input->remove_prefix(len);
  return Status::OK();
}

Status DecodeLogEntry(std::string_view input, SequenceNumber* sequence,
                      ValueType* type, std::string_view* key,
                      std::string_view* value) {
  Status s = GetFixed64(&input, sequence);
  if (!s.ok()) {
    return s;
  }
  if (*sequence > kMaxSequenceNumber) {
    return Status::Corruption("log entry sequence out of range");
  }
  if (input.empty()) {
    return Status::Corruption("truncated log entry");
  }
  const auto raw_type = static_cast<uint8_t>(input.front());
  input.remove_prefix(1);
  if (raw_type > static_cast<uint8_t>(ValueType::kValue)) {
    return Status::Corruption("unknown log entry type");
  }
  *type = static_cast<ValueType>(raw_type);
  s = GetLengthPrefixed(&input, key);
  if (!s.ok()) {
    return s;
  }
  s = GetLengthPrefixed(&input, value);
  if (!s.ok()) {
    return s;
  }
  if (!input.empty()) {
    return Status::Corruption("trailing bytes in log entry");
  }
  return Status::OK();
}

}  // namespace

void EncodeLogEntry(std::string* dst, SequenceNumber sequence, ValueType type,
                    std::string_view key, std::string_view value) {
  PutFixed64(dst, sequence);
  dst->push_back(static_cast<char>(type));
  PutVarint32(dst, static_cast<uint32_t>(key.size()));
  dst->append(key);
  PutVarint32(dst, static_cast<uint32_t>(value.size()));
  dst->append(value);
}

Status RecoverFromLog(const std::string& path, MemTable* memtable,
                      RecoveryStats* stats) {
  *stats = RecoveryStats();
  std::unique_ptr<SequentialFile> file;
  Status s = NewSequentialFile(path, &file);
  if (!s.ok()) {
    return s;
  }

  wal::Reader reader(std::move(file));
  std::string record;
  while (reader.ReadRecord(&record)) {
    SequenceNumber sequence = 0;
    ValueType type = ValueType::kValue;
    std::string_view key;
    std::string_view value;
    s = DecodeLogEntry(record, &sequence, &type, &key, &value);
    if (!s.ok()) {
      return s;
    }
    memtable->Add(sequence, type, key, value);
    ++stats->records;
    if (sequence > stats->last_sequence) {
      stats->last_sequence = sequence;
    }
  }
  stats->dropped_bytes = reader.dropped_bytes();
  stats->stop_reason = reader.stop_reason();
  return reader.status();
}

}  // namespace sediment
