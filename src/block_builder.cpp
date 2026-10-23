#include "sediment/block_builder.h"

#include <algorithm>
#include <cassert>

#include "sediment/coding.h"
#include "sediment/crc32c.h"

namespace sediment {

BlockBuilder::BlockBuilder(int restart_interval)
    : restart_interval_(restart_interval) {
  assert(restart_interval >= 1);
  restarts_.push_back(0);
}

void BlockBuilder::Add(std::string_view key, std::string_view value) {
  assert(!finished_);

  size_t shared = 0;
  if (entries_since_restart_ < restart_interval_) {
    const size_t limit = std::min(last_key_.size(), key.size());
    while (shared < limit && last_key_[shared] == key[shared]) {
      ++shared;
    }
  } else {
    restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
    entries_since_restart_ = 0;
  }
  const size_t non_shared = key.size() - shared;

  PutVarint32(&buffer_, static_cast<uint32_t>(shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(non_shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(value.size()));
  buffer_.append(key.data() + shared, non_shared);
  buffer_.append(value.data(), value.size());

  last_key_.assign(key.data(), key.size());
  ++entries_since_restart_;
}

std::string_view BlockBuilder::Finish() {
  assert(!finished_);
  for (const uint32_t restart : restarts_) {
    PutFixed32(&buffer_, restart);
  }
  PutFixed32(&buffer_, static_cast<uint32_t>(restarts_.size()));
  PutFixed32(&buffer_,
             crc32c::Mask(crc32c::Value(buffer_.data(), buffer_.size())));
  finished_ = true;
  return buffer_;
}

void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.assign(1, 0);
  entries_since_restart_ = 0;
  last_key_.clear();
  finished_ = false;
}

size_t BlockBuilder::CurrentSizeEstimate() const {
  if (finished_) {
    return buffer_.size();
  }
  return buffer_.size() + (restarts_.size() + 2) * sizeof(uint32_t);
}

}  // namespace sediment
