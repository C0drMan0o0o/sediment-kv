#include "sediment/table_builder.h"

#include <cassert>
#include <memory>

#include "sediment/coding.h"
#include "sediment/internal_key.h"

namespace sediment {

void Footer::EncodeTo(std::string* dst) const {
  const size_t start = dst->size();
  PutFixed64(dst, index_handle.offset);
  PutFixed64(dst, index_handle.size);
  PutFixed32(dst, version);
  PutFixed64(dst, kMagic);
  assert(dst->size() - start == kEncodedLength);
  (void)start;
}

Status Footer::DecodeFrom(std::string_view input, Footer* footer) {
  if (input.size() != kEncodedLength) {
    return Status::Corruption("footer has the wrong length");
  }
  Footer result;
  uint32_t version = 0;
  uint64_t magic = 0;
  Status s = GetFixed64(&input, &result.index_handle.offset);
  if (s.ok()) s = GetFixed64(&input, &result.index_handle.size);
  if (s.ok()) s = GetFixed32(&input, &version);
  if (s.ok()) s = GetFixed64(&input, &magic);
  if (!s.ok()) {
    return s;
  }
  if (magic != kMagic) {
    return Status::Corruption("bad table magic number");
  }
  if (version != kCurrentVersion) {
    return Status::Corruption("unsupported table version");
  }
  result.version = version;
  *footer = result;
  return Status::OK();
}

TableBuilder::TableBuilder(const TableBuilderOptions& options,
                           WritableFile* file)
    : options_(options),
      file_(file),
      data_block_(options.restart_interval),
      index_block_(options.restart_interval) {}

void TableBuilder::Add(std::string_view key, std::string_view value) {
  assert(!finished_);
  if (!status_.ok()) {
    return;
  }
  assert(num_entries_ == 0 || options_.comparator(largest_key_, key) < 0);

  if (num_entries_ == 0) {
    smallest_key_.assign(key.data(), key.size());
  }
  largest_key_.assign(key.data(), key.size());
  data_block_.Add(key, value);
  ++num_entries_;

  if (data_block_.CurrentSizeEstimate() >= options_.block_size) {
    FlushDataBlock();
  }
}

void TableBuilder::WriteBlock(std::string_view block, BlockHandle* handle) {
  handle->offset = offset_;
  handle->size = block.size();
  status_ = file_->Append(block);
  if (status_.ok()) {
    offset_ += block.size();
  }
}

void TableBuilder::FlushDataBlock() {
  if (!status_.ok() || data_block_.empty()) {
    return;
  }
  BlockHandle handle;
  WriteBlock(data_block_.Finish(), &handle);
  data_block_.Reset();
  if (!status_.ok()) {
    return;
  }
  // The block's last key is the table's largest key so far.
  std::string encoded_handle;
  PutVarint64(&encoded_handle, handle.offset);
  PutVarint64(&encoded_handle, handle.size);
  index_block_.Add(largest_key_, encoded_handle);
}

Status TableBuilder::Finish() {
  assert(!finished_);
  finished_ = true;
  FlushDataBlock();
  if (!status_.ok()) {
    return status_;
  }

  Footer footer;
  WriteBlock(index_block_.Finish(), &footer.index_handle);
  if (!status_.ok()) {
    return status_;
  }
  std::string encoded_footer;
  footer.EncodeTo(&encoded_footer);
  status_ = file_->Append(encoded_footer);
  if (status_.ok()) {
    offset_ += encoded_footer.size();
  }
  return status_;
}

Status WriteMemTableToTable(const MemTable& memtable,
                            const TableBuilderOptions& options,
                            const std::string& path, TableInfo* info) {
  std::unique_ptr<WritableFile> file;
  Status s = NewWritableFile(path, &file);
  if (!s.ok()) {
    return s;
  }

  TableBuilderOptions table_options = options;
  table_options.comparator = CompareInternalKeys;
  TableBuilder builder(table_options, file.get());
  MemTable::Iterator it(&memtable);
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    builder.Add(it.key(), it.value());
  }
  s = builder.Finish();
  if (s.ok()) s = file->Sync();
  if (s.ok()) s = file->Close();
  if (!s.ok()) {
    return s;
  }

  info->file_size = builder.FileSize();
  info->num_entries = builder.NumEntries();
  info->smallest_key.assign(builder.smallest_key());
  info->largest_key.assign(builder.largest_key());
  return Status::OK();
}

}  // namespace sediment
