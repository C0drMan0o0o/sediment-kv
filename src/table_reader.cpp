#include "sediment/table_reader.h"

#include <cassert>
#include <limits>
#include <utility>

#include "sediment/coding.h"

namespace sediment {

namespace {
Status ReadExactly(const RandomAccessFile& file, uint64_t offset, uint64_t n,
                   std::string* out) {
  if (n > std::numeric_limits<size_t>::max()) {
    return Status::Corruption("block too large");
  }
  out->resize(static_cast<size_t>(n));
  std::string_view result;
  Status s = file.Read(offset, out->size(), out->data(), &result);
  if (!s.ok()) {
    return s;
  }
  if (result.size() != out->size()) {
    return Status::Corruption("truncated table file");
  }
  // Read may hand back a view elsewhere than the scratch buffer.
  if (result.data() != out->data()) {
    out->assign(result);
  }
  return Status::OK();
}
}  // namespace

Table::Table(std::unique_ptr<RandomAccessFile> file, uint64_t data_size,
             KeyComparator comparator, std::string index_contents)
    : file_(std::move(file)),
      data_size_(data_size),
      comparator_(comparator),
      index_contents_(std::move(index_contents)),
      index_(index_contents_, comparator) {}

Status Table::Open(std::unique_ptr<RandomAccessFile> file, uint64_t file_size,
                   KeyComparator comparator, std::unique_ptr<Table>* table) {
  if (file_size < Footer::kEncodedLength) {
    return Status::Corruption("file too short to be a table");
  }
  const uint64_t data_size = file_size - Footer::kEncodedLength;

  std::string footer_bytes;
  Status s =
      ReadExactly(*file, data_size, Footer::kEncodedLength, &footer_bytes);
  if (!s.ok()) {
    return s;
  }
  Footer footer;
  s = Footer::DecodeFrom(footer_bytes, &footer);
  if (!s.ok()) {
    return s;
  }

  const BlockHandle& handle = footer.index_handle;
  if (handle.offset > data_size || handle.size > data_size - handle.offset) {
    return Status::Corruption("index block outside the file");
  }
  std::string index_contents;
  s = ReadExactly(*file, handle.offset, handle.size, &index_contents);
  if (!s.ok()) {
    return s;
  }

  std::unique_ptr<Table> result(new Table(
      std::move(file), data_size, comparator, std::move(index_contents)));
  if (!result->index_.status().ok()) {
    return result->index_.status();
  }
  *table = std::move(result);
  return Status::OK();
}

Status Table::ReadBlock(const BlockHandle& handle,
                        std::string* contents) const {
  if (handle.offset > data_size_ || handle.size > data_size_ - handle.offset) {
    return Status::Corruption("data block outside the file");
  }
  return ReadExactly(*file_, handle.offset, handle.size, contents);
}

Status Table::Seek(std::string_view target, std::string* key,
                   std::string* value) const {
  // The first index entry whose last key is >= target names the only block
  // that can hold the first entry >= target.
  BlockReader::Iterator index_it = index_.NewIterator();
  index_it.Seek(target);
  if (!index_it.Valid()) {
    return index_it.status().ok() ? Status::NotFound() : index_it.status();
  }

  std::string_view handle_bytes = index_it.value();
  BlockHandle handle;
  Status s = GetVarint64(&handle_bytes, &handle.offset);
  if (s.ok()) {
    s = GetVarint64(&handle_bytes, &handle.size);
  }
  if (!s.ok()) {
    return Status::Corruption("bad block handle in index");
  }

  std::string contents;
  s = ReadBlock(handle, &contents);
  if (!s.ok()) {
    return s;
  }
  const BlockReader block(contents, comparator_);
  if (!block.status().ok()) {
    return block.status();
  }
  BlockReader::Iterator it = block.NewIterator();
  it.Seek(target);
  if (!it.Valid()) {
    return it.status().ok() ? Status::NotFound() : it.status();
  }
  key->assign(it.key());
  value->assign(it.value());
  return Status::OK();
}

Status Table::Get(std::string_view key, std::string* value) const {
  std::string found_key;
  std::string found_value;
  Status s = Seek(key, &found_key, &found_value);
  if (!s.ok()) {
    return s;
  }
  if (comparator_(found_key, key) != 0) {
    return Status::NotFound();
  }
  *value = std::move(found_value);
  return Status::OK();
}

Status Table::Get(std::string_view user_key, SequenceNumber sequence,
                  std::string* value, LookupResult* result) const {
  assert(comparator_ == CompareInternalKeys);
  // kValue has the highest type number, so this probe sorts before every
  // version of `user_key` with sequence <= `sequence` (as in MemTable::Get).
  std::string probe;
  AppendInternalKey(
      &probe,
      {.user_key = user_key, .sequence = sequence, .type = ValueType::kValue});

  std::string found_key;
  std::string found_value;
  Status s = Seek(probe, &found_key, &found_value);
  if (s.IsNotFound()) {
    *result = LookupResult::kNotFound;
    return Status::OK();
  }
  if (!s.ok()) {
    return s;
  }
  ParsedInternalKey parsed;
  s = ParseInternalKey(found_key, &parsed);
  if (!s.ok()) {
    return s;
  }
  if (parsed.user_key != user_key) {
    *result = LookupResult::kNotFound;
  } else if (parsed.type == ValueType::kDeletion) {
    *result = LookupResult::kDeleted;
  } else {
    *value = std::move(found_value);
    *result = LookupResult::kFound;
  }
  return Status::OK();
}

}  // namespace sediment
