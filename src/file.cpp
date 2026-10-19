#include "sediment/file.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace sediment {
namespace {

Status PosixError(const std::string& context, int err) {
  return Status::IOError(context + ": " + std::strerror(err));
}

// Owns a file descriptor and the path it was opened from (for messages).
class PosixFd {
 public:
  PosixFd(std::string path, int fd) : path_(std::move(path)), fd_(fd) {}
  PosixFd(PosixFd&& other) noexcept
      : path_(std::move(other.path_)), fd_(std::exchange(other.fd_, -1)) {}
  PosixFd(const PosixFd&) = delete;
  PosixFd& operator=(const PosixFd&) = delete;
  PosixFd& operator=(PosixFd&&) = delete;
  ~PosixFd() {
    if (fd_ >= 0) {
      ::close(fd_);  // Failure is only reportable through an explicit Close().
    }
  }

  const std::string& path() const { return path_; }
  int fd() const { return fd_; }

  Status Close() {
    if (fd_ < 0) {
      return Status::OK();
    }
    const int fd = fd_;
    fd_ = -1;  // Never retry close(2): the descriptor is gone even on EINTR.
    if (::close(fd) != 0) {
      return PosixError(path_, errno);
    }
    return Status::OK();
  }

 private:
  std::string path_;
  int fd_;
};

Status SyncFd(const PosixFd& file) {
#ifdef __APPLE__
  // On macOS fsync(2) only pushes data to the drive, which may keep it in a
  // volatile write cache. F_FULLFSYNC asks the drive to flush that cache too.
  // It is unsupported on some file systems, so fall back to fsync there.
  if (::fcntl(file.fd(), F_FULLFSYNC) == 0) {
    return Status::OK();
  }
#endif
  while (::fsync(file.fd()) != 0) {
    if (errno != EINTR) {
      return PosixError(file.path(), errno);
    }
  }
  return Status::OK();
}

class PosixWritableFile final : public WritableFile {
 public:
  explicit PosixWritableFile(PosixFd file) : file_(std::move(file)) {}

  Status Append(std::string_view data) override {
    if (file_.fd() < 0) {
      return Status::InvalidArgument(file_.path() + ": file is closed");
    }
    while (!data.empty()) {
      const ssize_t written = ::write(file_.fd(), data.data(), data.size());
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        return PosixError(file_.path(), errno);
      }
      data.remove_prefix(static_cast<size_t>(written));
    }
    return Status::OK();
  }

  Status Sync() override {
    if (file_.fd() < 0) {
      return Status::InvalidArgument(file_.path() + ": file is closed");
    }
    return SyncFd(file_);
  }

  Status Close() override { return file_.Close(); }

 private:
  PosixFd file_;
};

class PosixSequentialFile final : public SequentialFile {
 public:
  explicit PosixSequentialFile(PosixFd file) : file_(std::move(file)) {}

  Status Read(size_t n, char* scratch, std::string_view* result) override {
    for (;;) {
      const ssize_t got = ::read(file_.fd(), scratch, n);
      if (got < 0) {
        if (errno == EINTR) {
          continue;
        }
        *result = {};
        return PosixError(file_.path(), errno);
      }
      *result = std::string_view(scratch, static_cast<size_t>(got));
      return Status::OK();
    }
  }

  Status Skip(uint64_t n) override {
    if (::lseek(file_.fd(), static_cast<off_t>(n), SEEK_CUR) < 0) {
      return PosixError(file_.path(), errno);
    }
    return Status::OK();
  }

 private:
  PosixFd file_;
};

class PosixRandomAccessFile final : public RandomAccessFile {
 public:
  explicit PosixRandomAccessFile(PosixFd file) : file_(std::move(file)) {}

  Status Read(uint64_t offset, size_t n, char* scratch,
              std::string_view* result) const override {
    size_t total = 0;
    while (total < n) {
      const ssize_t got = ::pread(file_.fd(), scratch + total, n - total,
                                  static_cast<off_t>(offset + total));
      if (got < 0) {
        if (errno == EINTR) {
          continue;
        }
        *result = {};
        return PosixError(file_.path(), errno);
      }
      if (got == 0) {
        break;  // End of file.
      }
      total += static_cast<size_t>(got);
    }
    *result = std::string_view(scratch, total);
    return Status::OK();
  }

 private:
  PosixFd file_;
};

// Open `path` with `flags` and wrap the descriptor in an `Impl`.
template <typename Impl, typename Base>
Status OpenAs(const std::string& path, int flags,
              std::unique_ptr<Base>* result) {
  int fd = -1;
  do {
    // NOLINTNEXTLINE(bugprone-signed-bitwise): O_* flags are int macros.
    fd = ::open(path.c_str(), flags | O_CLOEXEC, 0644);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) {
    result->reset();
    return PosixError(path, errno);
  }
  *result = std::make_unique<Impl>(PosixFd(path, fd));
  return Status::OK();
}

}  // namespace

Status NewWritableFile(const std::string& path,
                       std::unique_ptr<WritableFile>* result) {
  return OpenAs<PosixWritableFile>(path, O_WRONLY | O_CREAT | O_TRUNC, result);
}

Status NewSequentialFile(const std::string& path,
                         std::unique_ptr<SequentialFile>* result) {
  return OpenAs<PosixSequentialFile>(path, O_RDONLY, result);
}

Status NewRandomAccessFile(const std::string& path,
                           std::unique_ptr<RandomAccessFile>* result) {
  return OpenAs<PosixRandomAccessFile>(path, O_RDONLY, result);
}

}  // namespace sediment
