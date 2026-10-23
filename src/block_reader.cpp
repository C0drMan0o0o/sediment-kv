#include "sediment/block_reader.h"

#include <algorithm>
#include <cstring>

#include "sediment/coding.h"
#include "sediment/crc32c.h"

namespace sediment {

namespace {
constexpr size_t kFixed32Size = sizeof(uint32_t);
// num_restarts + crc.
constexpr size_t kFooterSize = 2 * kFixed32Size;

uint32_t DecodeFixed32At(const char* p) {
  std::string_view in(p, kFixed32Size);
  uint32_t v = 0;
  (void)GetFixed32(&in, &v);
  return v;
}
}  // namespace

int BytewiseCompare(std::string_view a, std::string_view b) {
  return a.compare(b);
}

BlockReader::BlockReader(std::string_view contents, KeyComparator comparator)
    : comparator_(comparator) {
  if (contents.size() < kFooterSize + kFixed32Size) {
    status_ = Status::Corruption("block too short");
    return;
  }
  const size_t crc_pos = contents.size() - kFixed32Size;
  const uint32_t stored =
      crc32c::Unmask(DecodeFixed32At(contents.data() + crc_pos));
  if (stored != crc32c::Value(contents.data(), crc_pos)) {
    status_ = Status::Corruption("block checksum mismatch");
    return;
  }
  num_restarts_ = DecodeFixed32At(contents.data() + crc_pos - kFixed32Size);
  const size_t max_restarts = (crc_pos - kFixed32Size) / kFixed32Size;
  if (num_restarts_ == 0 || num_restarts_ > max_restarts) {
    status_ = Status::Corruption("bad restart count");
    return;
  }
  const size_t restarts_bytes =
      static_cast<size_t>(num_restarts_) * kFixed32Size;
  const size_t entries_size = crc_pos - kFixed32Size - restarts_bytes;
  contents_ = contents.substr(0, entries_size);
  restarts_ = contents.data() + entries_size;
}

BlockReader::Iterator BlockReader::NewIterator() const {
  return Iterator(this);
}

BlockReader::Iterator::Iterator(const BlockReader* block) : block_(block) {
  if (!block->status_.ok()) {
    status_ = block->status_;
  }
}

uint32_t BlockReader::Iterator::RestartOffset(uint32_t index) const {
  return DecodeFixed32At(block_->restarts_ +
                         static_cast<size_t>(index) * kFixed32Size);
}

void BlockReader::Iterator::SeekToRestart(uint32_t index) {
  key_.clear();
  value_ = {};
  next_ = RestartOffset(index);
}

void BlockReader::Iterator::Invalidate(Status status) {
  valid_ = false;
  key_.clear();
  value_ = {};
  if (!status.ok()) {
    status_ = std::move(status);
  }
}

bool BlockReader::Iterator::ParseNext() {
  const std::string_view entries = block_->contents_;
  if (next_ >= entries.size()) {
    Invalidate(Status::OK());
    return false;
  }
  std::string_view in = entries.substr(next_);
  uint32_t shared = 0;
  uint32_t non_shared = 0;
  uint32_t value_length = 0;
  if (!GetVarint32(&in, &shared).ok() || !GetVarint32(&in, &non_shared).ok() ||
      !GetVarint32(&in, &value_length).ok() || shared > key_.size() ||
      in.size() < static_cast<uint64_t>(non_shared) + value_length) {
    Invalidate(Status::Corruption("bad block entry"));
    return false;
  }
  key_.resize(shared);
  key_.append(in.data(), non_shared);
  value_ = in.substr(non_shared, value_length);
  next_ = static_cast<size_t>(in.data() + non_shared + value_length -
                              entries.data());
  valid_ = true;
  return true;
}

void BlockReader::Iterator::SeekToFirst() {
  if (!status_.ok()) {
    return;
  }
  SeekToRestart(0);
  ParseNext();
}

void BlockReader::Iterator::Next() {
  ParseNext();
}

void BlockReader::Iterator::Seek(std::string_view target) {
  if (!status_.ok()) {
    return;
  }
  // Find the last restart whose (full) key is < target; every entry before it
  // is smaller than target too, so the scan can start there.
  uint32_t lo = 0;
  uint32_t hi = block_->num_restarts_ - 1;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo + 1) / 2;
    SeekToRestart(mid);
    if (!ParseNext()) {
      if (status_.ok()) {
        status_ = Status::Corruption("restart offset past end of block");
      }
      return;
    }
    if (block_->comparator_(key_, target) < 0) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  SeekToRestart(lo);
  while (ParseNext()) {
    if (block_->comparator_(key_, target) >= 0) {
      return;
    }
  }
}

}  // namespace sediment
