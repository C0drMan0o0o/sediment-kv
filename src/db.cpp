#include "sediment/db.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "sediment/filename.h"

namespace sediment {
namespace {

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

// The number for the next new file: one past every numbered file in `dir`, so
// that no existing file is overwritten.
Status NextFileNumber(const std::string& dir, uint64_t* number) {
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
    }
  }
  if (max_number == std::numeric_limits<uint64_t>::max()) {
    // Wrapping to 0 would truncate any existing file numbered 0.
    return Status::Corruption(dir + ": file numbers exhausted");
  }
  *number = max_number + 1;
  return Status::OK();
}

}  // namespace

DB::DB(std::unique_ptr<FileLock> lock, std::unique_ptr<wal::Writer> log)
    : lock_(std::move(lock)), log_(std::move(log)) {}

Status DB::Open(const Options& /*options*/, const std::string& path,
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

  // Existing logs are left as they are; numbering past them keeps them from
  // being truncated by the new one.
  uint64_t log_number = 0;
  s = NextFileNumber(path, &log_number);
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

  db->reset(new DB(std::move(lock),
                   std::make_unique<wal::Writer>(std::move(log_file))));
  return Status::OK();
}

}  // namespace sediment
