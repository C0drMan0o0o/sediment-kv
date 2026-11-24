#include "sediment/db.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sediment/filename.h"
#include "sediment/recovery.h"

namespace sediment {
namespace {

// Lengths are stored as varint32, and a key also needs room for the 8-byte tag;
// see EncodeLogEntry and MemTable::Add.
constexpr size_t kMaxKeySize = UINT32_MAX - 8;
constexpr size_t kMaxValueSize = UINT32_MAX;

// The directory containing `path`: "." for a bare name, "/" for a top-level
// one. Trailing slashes on `path` are ignored.
std::string ParentDir(std::string path) {
  while (path.size() > 1 && path.back() == '/') {
    path.pop_back();
  }
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos) {
    return ".";
  }
  return slash == 0 ? "/" : path.substr(0, slash);
}

// Set `*logs` to the numbers of the logs in `dir`, oldest first, and
// `*next_number` to the number for the next new file: one past every numbered
// file, so that no existing file is overwritten.
Status ScanDir(const std::string& dir, std::vector<uint64_t>* logs,
               uint64_t* next_number) {
  std::vector<std::string> children;
  Status s = GetChildren(dir, &children);
  if (!s.ok()) {
    return s;
  }
  uint64_t max_number = 0;
  for (const std::string& name : children) {
    uint64_t file_number = 0;
    FileType type{};
    if (ParseFileName(name, &file_number, &type)) {
      max_number = std::max(max_number, file_number);
      if (type == FileType::kWal) {
        logs->push_back(file_number);
      }
    }
  }
  if (max_number == std::numeric_limits<uint64_t>::max()) {
    // Wrapping to 0 would truncate any existing file numbered 0.
    return Status::Corruption(dir + ": file numbers exhausted");
  }
  std::ranges::sort(*logs);
  *next_number = max_number + 1;
  return Status::OK();
}

// Replay the logs numbered `logs`, in order, into `mem`, and set
// `*last_sequence` to the highest sequence number found (0 if none).
Status ReplayLogs(const std::string& dir, const std::vector<uint64_t>& logs,
                  MemTable* mem, SequenceNumber* last_sequence) {
  *last_sequence = 0;
  for (const uint64_t number : logs) {
    RecoveryStats stats;
    Status s = RecoverFromLog(WalFileName(dir, number), mem, &stats);
    if (!s.ok()) {
      return s;
    }
    *last_sequence = std::max(*last_sequence, stats.last_sequence);
  }
  return Status::OK();
}

}  // namespace

DB::DB(const Options& options, std::unique_ptr<FileLock> lock,
       std::unique_ptr<wal::Writer> log, std::unique_ptr<MemTable> mem,
       SequenceNumber last_sequence)
    : options_(options),
      lock_(std::move(lock)),
      log_(std::move(log)),
      mem_(std::move(mem)),
      last_sequence_(last_sequence) {}

Status DB::Open(const Options& options, const std::string& path,
                std::unique_ptr<DB>* db) {
  db->reset();

  // The directory's entry lives in its parent, so sync the parent for the
  // directory itself to survive a crash. This happens on every open, not only
  // when the directory is new: an earlier open may have created it and then
  // failed or crashed before its sync.
  Status s = CreateDirIfMissing(path);
  if (s.ok()) {
    s = SyncDir(ParentDir(path));
  }
  if (!s.ok()) {
    return s;
  }

  std::unique_ptr<FileLock> lock;
  s = LockFile(LockFileName(path), &lock);
  if (!s.ok()) {
    return s;
  }

  // Replayed logs are kept: until the memtable is flushed to a table they hold
  // its only durable copy. Numbering past them keeps them from being truncated
  // by the new log.
  std::vector<uint64_t> logs;
  uint64_t log_number = 0;
  s = ScanDir(path, &logs, &log_number);
  auto mem = std::make_unique<MemTable>();
  SequenceNumber last_sequence = 0;
  if (s.ok()) {
    s = ReplayLogs(path, logs, mem.get(), &last_sequence);
  }
  std::unique_ptr<WritableFile> log_file;
  if (s.ok()) {
    s = NewWritableFile(WalFileName(path, log_number), &log_file);
  }
  // Make the new log's directory entry durable before any write to it is
  // acknowledged.
  if (s.ok()) {
    s = SyncDir(path);
  }
  if (!s.ok()) {
    return s;
  }

  db->reset(new DB(options, std::move(lock),
                   std::make_unique<wal::Writer>(std::move(log_file)),
                   std::move(mem), last_sequence));
  return Status::OK();
}

Status DB::Put(std::string_view key, std::string_view value) {
  return Write(ValueType::kValue, key, value);
}

Status DB::Delete(std::string_view key) {
  return Write(ValueType::kDeletion, key, {});
}

Status DB::Get(std::string_view key, std::string* value) const {
  // Put rejects such a key, and MemTable::Get cannot encode it.
  if (key.size() > kMaxKeySize) {
    return Status::NotFound();
  }
  if (mem_->Get(key, last_sequence_, value) == MemTable::GetResult::kFound) {
    return Status::OK();
  }
  return Status::NotFound();
}

Status DB::Write(ValueType type, std::string_view key, std::string_view value) {
  if (key.size() > kMaxKeySize) {
    return Status::InvalidArgument("key too large");
  }
  if (value.size() > kMaxValueSize) {
    return Status::InvalidArgument("value too large");
  }

  // Log first, so the memtable never holds a write the log might lack.
  const SequenceNumber sequence = last_sequence_ + 1;
  entry_.clear();
  EncodeLogEntry(&entry_, sequence, type, key, value);
  Status s = log_->AddRecord(entry_, options_.sync_writes);
  if (!s.ok()) {
    return s;
  }
  mem_->Add(sequence, type, key, value);
  last_sequence_ = sequence;
  return Status::OK();
}

}  // namespace sediment
