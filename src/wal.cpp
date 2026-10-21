#include "sediment/wal.h"

#include <algorithm>
#include <array>
#include <utility>

#include "sediment/coding.h"
#include "sediment/crc32c.h"

namespace sediment::wal {

Writer::Writer(std::unique_ptr<WritableFile> file) : file_(std::move(file)) {}

Status Writer::AddRecord(std::string_view payload, bool sync) {
  if (!error_.ok()) {
    return error_;
  }
  if (payload.size() > kMaxPayloadSize) {
    return Status::InvalidArgument("wal record too large");
  }

  const auto type = static_cast<char>(RecordType::kFull);
  uint32_t crc = crc32c::Extend(0, &type, 1);
  crc = crc32c::Extend(crc, payload.data(), payload.size());

  std::array<char, kHeaderSize> header;
  EncodeFixed32(header.data(), crc32c::Mask(crc));
  EncodeFixed32(header.data() + 4, static_cast<uint32_t>(payload.size()));
  header[8] = type;

  Status s = file_->Append({header.data(), header.size()});
  if (s.ok()) {
    s = file_->Append(payload);
  }
  if (s.ok() && sync) {
    s = file_->Sync();
  }
  if (!s.ok()) {
    error_ = s;
  }
  return s;
}

Status Writer::Sync() {
  return file_->Sync();
}

Status Writer::Close() {
  return file_->Close();
}

namespace {
// Payloads are read in chunks so that a corrupt length field cannot trigger a
// huge allocation before the short read reveals the damage.
constexpr size_t kReadChunk = size_t{64} * 1024;
}  // namespace

Reader::Reader(std::unique_ptr<SequentialFile> file) : file_(std::move(file)) {}

size_t Reader::ReadFully(size_t n, char* dst) {
  size_t total = 0;
  while (total < n) {
    std::string_view got;
    Status s = file_->Read(n - total, dst + total, &got);
    if (!s.ok()) {
      status_ = std::move(s);
      break;
    }
    if (got.empty()) {
      break;
    }
    total += got.size();
  }
  return total;
}

// Mark the log finished. `consumed` is how many bytes of the damaged frame were
// already read; the rest of the file is read and counted as dropped too.
bool Reader::Stop(StopReason reason, uint64_t consumed) {
  done_ = true;
  stop_reason_ = reason;
  dropped_bytes_ = consumed;
  std::string scratch(kReadChunk, '\0');
  while (status_.ok()) {
    const size_t n = ReadFully(scratch.size(), scratch.data());
    dropped_bytes_ += n;
    if (n < scratch.size()) {
      break;
    }
  }
  if (!status_.ok()) {
    stop_reason_ = StopReason::kIOError;
  }
  return false;
}

bool Reader::ReadRecord(std::string* record) {
  if (done_) {
    return false;
  }

  std::array<char, kHeaderSize> header;
  const size_t header_got = ReadFully(header.size(), header.data());
  if (!status_.ok()) {
    return Stop(StopReason::kIOError, header_got);
  }
  if (header_got == 0) {
    done_ = true;
    return false;
  }
  if (header_got < header.size()) {
    return Stop(StopReason::kTruncatedHeader, header_got);
  }

  std::string_view fields(header.data(), 8);
  uint32_t stored_crc = 0;
  uint32_t length = 0;
  // Cannot fail: `fields` holds exactly the two fixed32 values.
  (void)GetFixed32(&fields, &stored_crc);
  (void)GetFixed32(&fields, &length);
  const char type = header[8];
  if (static_cast<RecordType>(type) != RecordType::kFull) {
    return Stop(StopReason::kBadType, header_got);
  }

  std::string payload;
  while (payload.size() < length) {
    const size_t want = std::min<size_t>(kReadChunk, length - payload.size());
    const size_t old_size = payload.size();
    payload.resize(old_size + want);
    const size_t got = ReadFully(want, payload.data() + old_size);
    payload.resize(old_size + got);
    if (!status_.ok()) {
      return Stop(StopReason::kIOError, header_got + payload.size());
    }
    if (got < want) {
      return Stop(StopReason::kTruncatedPayload, header_got + payload.size());
    }
  }

  uint32_t crc = crc32c::Extend(0, &type, 1);
  crc = crc32c::Extend(crc, payload.data(), payload.size());
  if (crc32c::Unmask(stored_crc) != crc) {
    return Stop(StopReason::kBadChecksum, header_got + payload.size());
  }

  *record = std::move(payload);
  return true;
}

}  // namespace sediment::wal
