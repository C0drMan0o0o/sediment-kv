#include "sediment/wal.h"

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

}  // namespace sediment::wal
