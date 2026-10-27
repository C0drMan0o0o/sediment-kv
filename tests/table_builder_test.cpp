#include "sediment/table_builder.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "sediment/block_reader.h"
#include "sediment/coding.h"
#include "sediment/internal_key.h"
#include "sediment/memtable.h"

namespace sediment {
namespace {

using Entries = std::vector<std::pair<std::string, std::string>>;

class TableBuilderTest : public testing::Test {
 protected:
  void SetUp() override {
    const auto* info = testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() /
           (std::string("sediment_table_test_") + info->name());
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }

  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::string Path(const std::string& name) const {
    return (dir_ / name).string();
  }

  static std::string ReadAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
  }

  // Build a table from `entries` and return the file's bytes.
  std::string BuildTable(const Entries& entries,
                         const TableBuilderOptions& options) {
    std::unique_ptr<WritableFile> file;
    EXPECT_TRUE(NewWritableFile(Path("t.sst"), &file).ok());
    TableBuilder builder(options, file.get());
    for (const auto& [k, v] : entries) {
      builder.Add(k, v);
    }
    EXPECT_TRUE(builder.Finish().ok());
    EXPECT_TRUE(file->Close().ok());
    const std::string bytes = ReadAll(Path("t.sst"));
    EXPECT_EQ(bytes.size(), builder.FileSize());
    EXPECT_EQ(builder.NumEntries(), entries.size());
    return bytes;
  }

  // Walk the table the way a reader would and return every entry, checking
  // the index along the way. Returns the number of data blocks via *blocks.
  static Entries ReadBack(const std::string& bytes, KeyComparator comparator,
                          size_t* blocks) {
    Entries result;
    *blocks = 0;
    EXPECT_GE(bytes.size(), Footer::kEncodedLength);
    Footer footer;
    EXPECT_TRUE(Footer::DecodeFrom(std::string_view(bytes).substr(
                                       bytes.size() - Footer::kEncodedLength),
                                   &footer)
                    .ok());
    EXPECT_EQ(footer.version, Footer::kCurrentVersion);
    EXPECT_EQ(footer.index_handle.offset + footer.index_handle.size +
                  Footer::kEncodedLength,
              bytes.size());

    BlockReader index(std::string_view(bytes).substr(footer.index_handle.offset,
                                                     footer.index_handle.size),
                      comparator);
    EXPECT_TRUE(index.status().ok());
    auto index_it = index.NewIterator();
    uint64_t expected_offset = 0;
    for (index_it.SeekToFirst(); index_it.Valid(); index_it.Next()) {
      std::string_view handle_bytes = index_it.value();
      BlockHandle handle;
      EXPECT_TRUE(GetVarint64(&handle_bytes, &handle.offset).ok());
      EXPECT_TRUE(GetVarint64(&handle_bytes, &handle.size).ok());
      EXPECT_EQ(handle.offset, expected_offset);  // blocks are contiguous
      expected_offset += handle.size;
      ++*blocks;

      BlockReader block(
          std::string_view(bytes).substr(handle.offset, handle.size),
          comparator);
      EXPECT_TRUE(block.status().ok());
      auto it = block.NewIterator();
      std::string last;
      for (it.SeekToFirst(); it.Valid(); it.Next()) {
        result.emplace_back(std::string(it.key()), std::string(it.value()));
        last = std::string(it.key());
      }
      EXPECT_EQ(last, std::string(index_it.key()));  // index key = last key
    }
    EXPECT_TRUE(index_it.status().ok());
    EXPECT_EQ(expected_offset, footer.index_handle.offset);
    return result;
  }

  std::filesystem::path dir_;
};

Entries SortedEntries(int n) {
  Entries entries;
  for (int i = 0; i < n; ++i) {
    std::array<char, 32> key{};
    std::snprintf(key.data(), key.size(), "key%08d", i);
    entries.emplace_back(key.data(), "value" + std::to_string(i));
  }
  return entries;
}

TEST(Footer, RoundTripsAndHasFixedLength) {
  Footer footer;
  footer.index_handle = {.offset = 0x0102030405060708, .size = 99};
  std::string encoded;
  footer.EncodeTo(&encoded);
  ASSERT_EQ(encoded.size(), Footer::kEncodedLength);

  Footer decoded;
  ASSERT_TRUE(Footer::DecodeFrom(encoded, &decoded).ok());
  EXPECT_EQ(decoded.index_handle, footer.index_handle);
  EXPECT_EQ(decoded.version, Footer::kCurrentVersion);
}

TEST(Footer, EndsWithMagicNumber) {
  std::string encoded;
  Footer().EncodeTo(&encoded);
  EXPECT_EQ(encoded.substr(20), std::string("sediment"));
}

TEST(Footer, RejectsBadLengthMagicAndVersion) {
  Footer footer;
  std::string encoded;
  footer.EncodeTo(&encoded);
  Footer out;

  EXPECT_TRUE(
      Footer::DecodeFrom(encoded.substr(1), &out).IsCorruption());  // short
  EXPECT_TRUE(Footer::DecodeFrom(encoded + "x", &out).IsCorruption());

  std::string bad_magic = encoded;
  bad_magic[27] = static_cast<char>(bad_magic[27] + 1);
  EXPECT_TRUE(Footer::DecodeFrom(bad_magic, &out).IsCorruption());

  std::string bad_version = encoded;
  bad_version[16] = 99;
  EXPECT_TRUE(Footer::DecodeFrom(bad_version, &out).IsCorruption());
}

TEST_F(TableBuilderTest, EmptyTableHasEmptyIndexAndFooter) {
  const std::string bytes = BuildTable({}, TableBuilderOptions{});
  size_t blocks = 0;
  EXPECT_TRUE(ReadBack(bytes, BytewiseCompare, &blocks).empty());
  EXPECT_EQ(blocks, 0U);
}

TEST_F(TableBuilderTest, SingleBlockRoundTrip) {
  const Entries entries = SortedEntries(10);
  const std::string bytes = BuildTable(entries, TableBuilderOptions{});
  size_t blocks = 0;
  EXPECT_EQ(ReadBack(bytes, BytewiseCompare, &blocks), entries);
  EXPECT_EQ(blocks, 1U);
}

TEST_F(TableBuilderTest, SplitsIntoManyDataBlocks) {
  const Entries entries = SortedEntries(2000);
  TableBuilderOptions options;
  options.block_size = 256;
  const std::string bytes = BuildTable(entries, options);
  size_t blocks = 0;
  EXPECT_EQ(ReadBack(bytes, BytewiseCompare, &blocks), entries);
  EXPECT_GT(blocks, 10U);
}

TEST_F(TableBuilderTest, BlockSizeOfOneGivesOneEntryPerBlock) {
  const Entries entries = SortedEntries(5);
  TableBuilderOptions options;
  options.block_size = 1;
  const std::string bytes = BuildTable(entries, options);
  size_t blocks = 0;
  EXPECT_EQ(ReadBack(bytes, BytewiseCompare, &blocks), entries);
  EXPECT_EQ(blocks, 5U);
}

TEST_F(TableBuilderTest, TracksSmallestAndLargestKey) {
  std::unique_ptr<WritableFile> file;
  ASSERT_TRUE(NewWritableFile(Path("t.sst"), &file).ok());
  TableBuilderOptions options;
  options.block_size = 64;
  TableBuilder builder(options, file.get());
  EXPECT_TRUE(builder.smallest_key().empty());
  EXPECT_TRUE(builder.largest_key().empty());

  for (const auto& [k, v] : SortedEntries(100)) {
    builder.Add(k, v);
  }
  EXPECT_EQ(builder.smallest_key(), "key00000000");
  EXPECT_EQ(builder.largest_key(), "key00000099");
  EXPECT_EQ(builder.NumEntries(), 100U);
  ASSERT_TRUE(builder.Finish().ok());
  EXPECT_EQ(builder.smallest_key(), "key00000000");
  EXPECT_EQ(builder.largest_key(), "key00000099");
}

TEST_F(TableBuilderTest, FileSizeCountsEveryWrittenByte) {
  std::unique_ptr<WritableFile> file;
  ASSERT_TRUE(NewWritableFile(Path("t.sst"), &file).ok());
  TableBuilderOptions options;
  options.block_size = 128;
  TableBuilder builder(options, file.get());
  for (const auto& [k, v] : SortedEntries(50)) {
    builder.Add(k, v);
  }
  ASSERT_TRUE(builder.Finish().ok());
  ASSERT_TRUE(file->Close().ok());
  EXPECT_EQ(builder.FileSize(), std::filesystem::file_size(Path("t.sst")));
}

TEST_F(TableBuilderTest, MemTableWritesToSstFile) {
  MemTable memtable;
  memtable.Add(1, ValueType::kValue, "apple", "red");
  memtable.Add(2, ValueType::kValue, "banana", "yellow");
  memtable.Add(3, ValueType::kValue, "apple", "green");  // newer apple
  memtable.Add(4, ValueType::kDeletion, "banana", "");
  memtable.Add(5, ValueType::kValue, "cherry", "dark");

  TableBuilderOptions options;
  options.block_size = 32;
  TableInfo info;
  ASSERT_TRUE(
      WriteMemTableToTable(memtable, options, Path("000001.sst"), &info).ok());

  const std::string bytes = ReadAll(Path("000001.sst"));
  EXPECT_EQ(bytes.size(), info.file_size);
  EXPECT_EQ(info.num_entries, 5U);

  // Newest version of each user key comes first; tombstones are kept.
  size_t blocks = 0;
  const Entries got = ReadBack(bytes, CompareInternalKeys, &blocks);
  ASSERT_EQ(got.size(), 5U);
  const std::vector<
      std::tuple<std::string, SequenceNumber, ValueType, std::string>>
      expected = {
          {"apple", 3, ValueType::kValue, "green"},
          {"apple", 1, ValueType::kValue, "red"},
          {"banana", 4, ValueType::kDeletion, ""},
          {"banana", 2, ValueType::kValue, "yellow"},
          {"cherry", 5, ValueType::kValue, "dark"},
  };
  for (size_t i = 0; i < got.size(); ++i) {
    ParsedInternalKey parsed;
    ASSERT_TRUE(ParseInternalKey(got[i].first, &parsed).ok());
    EXPECT_EQ(std::string(parsed.user_key), std::get<0>(expected[i]));
    EXPECT_EQ(parsed.sequence, std::get<1>(expected[i]));
    EXPECT_EQ(parsed.type, std::get<2>(expected[i]));
    EXPECT_EQ(got[i].second, std::get<3>(expected[i]));
  }
  EXPECT_GT(blocks, 1U);

  EXPECT_EQ(info.smallest_key, got.front().first);
  EXPECT_EQ(info.largest_key, got.back().first);
}

TEST_F(TableBuilderTest, EmptyMemTableWritesValidTable) {
  MemTable memtable;
  TableInfo info;
  ASSERT_TRUE(WriteMemTableToTable(memtable, TableBuilderOptions{},
                                   Path("e.sst"), &info)
                  .ok());
  EXPECT_EQ(info.num_entries, 0U);
  size_t blocks = 0;
  EXPECT_TRUE(
      ReadBack(ReadAll(Path("e.sst")), CompareInternalKeys, &blocks).empty());
}

TEST_F(TableBuilderTest, WriteMemTableReportsUnwritablePath) {
  MemTable memtable;
  TableInfo info;
  EXPECT_TRUE(WriteMemTableToTable(memtable, TableBuilderOptions{},
                                   Path("missing/dir/x.sst"), &info)
                  .IsIOError());
}

}  // namespace
}  // namespace sediment
