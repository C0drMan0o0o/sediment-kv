#include "sediment/table_reader.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "sediment/internal_key.h"
#include "sediment/memtable.h"

namespace sediment {
namespace {

using Entries = std::vector<std::pair<std::string, std::string>>;

Entries SortedEntries(int n) {
  Entries entries;
  for (int i = 0; i < n; ++i) {
    std::array<char, 32> key{};
    std::snprintf(key.data(), key.size(), "key%08d", i);
    entries.emplace_back(key.data(), "value" + std::to_string(i));
  }
  return entries;
}

class TableReaderTest : public testing::Test {
 protected:
  void SetUp() override {
    const auto* info = testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() /
           (std::string("sediment_table_reader_test_") + info->name());
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }

  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::string Path(const std::string& name) const {
    return (dir_ / name).string();
  }

  void BuildTable(const Entries& entries, size_t block_size) {
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(NewWritableFile(Path("t.sst"), &file).ok());
    TableBuilderOptions options;
    options.block_size = block_size;
    TableBuilder builder(options, file.get());
    for (const auto& [k, v] : entries) {
      builder.Add(k, v);
    }
    ASSERT_TRUE(builder.Finish().ok());
    ASSERT_TRUE(file->Close().ok());
  }

  Status OpenTable(std::unique_ptr<Table>* table,
                   KeyComparator comparator = BytewiseCompare) {
    std::unique_ptr<RandomAccessFile> file;
    Status s = NewRandomAccessFile(Path("t.sst"), &file);
    if (!s.ok()) {
      return s;
    }
    return Table::Open(std::move(file),
                       std::filesystem::file_size(Path("t.sst")), comparator,
                       table);
  }

  // Flip one byte of the table file in place.
  void FlipByte(uint64_t offset) {
    std::fstream f(Path("t.sst"),
                   std::ios::in | std::ios::out | std::ios::binary);
    f.seekg(static_cast<std::streamoff>(offset));
    char c = 0;
    f.get(c);
    const auto flipped = static_cast<unsigned char>(c) ^ 0x01U;
    f.seekp(static_cast<std::streamoff>(offset));
    f.put(static_cast<char>(flipped));
  }

  std::filesystem::path dir_;
};

TEST_F(TableReaderTest, ReadsBackEveryKeyAcrossManyBlocks) {
  const Entries entries = SortedEntries(2000);
  BuildTable(entries, 256);
  std::unique_ptr<Table> table;
  ASSERT_TRUE(OpenTable(&table).ok());
  for (const auto& [k, v] : entries) {
    std::string value;
    ASSERT_TRUE(table->Get(k, &value).ok()) << k;
    EXPECT_EQ(value, v);
  }
}

TEST_F(TableReaderTest, MissingKeysAreNotFound) {
  BuildTable(SortedEntries(500), 128);
  std::unique_ptr<Table> table;
  ASSERT_TRUE(OpenTable(&table).ok());
  std::string value;
  EXPECT_TRUE(table->Get("a", &value).IsNotFound());             // before first
  EXPECT_TRUE(table->Get("key00000000x", &value).IsNotFound());  // between
  EXPECT_TRUE(table->Get("key00000250a", &value).IsNotFound());  // between
  EXPECT_TRUE(table->Get("zzz", &value).IsNotFound());           // after last
}

TEST_F(TableReaderTest, EmptyTableFindsNothing) {
  BuildTable({}, 4096);
  std::unique_ptr<Table> table;
  ASSERT_TRUE(OpenTable(&table).ok());
  std::string value;
  EXPECT_TRUE(table->Get("k", &value).IsNotFound());
}

TEST_F(TableReaderTest, OneEntryPerBlock) {
  const Entries entries = SortedEntries(20);
  BuildTable(entries, 1);
  std::unique_ptr<Table> table;
  ASSERT_TRUE(OpenTable(&table).ok());
  for (const auto& [k, v] : entries) {
    std::string value;
    ASSERT_TRUE(table->Get(k, &value).ok());
    EXPECT_EQ(value, v);
  }
}

TEST_F(TableReaderTest, ReadsMemTableFlushedByInternalKey) {
  MemTable memtable;
  memtable.Add(1, ValueType::kValue, "apple", "red");
  memtable.Add(2, ValueType::kDeletion, "apple", "");
  memtable.Add(3, ValueType::kValue, "cherry", "dark");
  TableBuilderOptions options;
  options.block_size = 16;
  TableInfo info;
  ASSERT_TRUE(
      WriteMemTableToTable(memtable, options, Path("t.sst"), &info).ok());

  std::unique_ptr<Table> table;
  ASSERT_TRUE(OpenTable(&table, CompareInternalKeys).ok());
  std::string value;
  std::string key;
  AppendInternalKey(
      &key, {.user_key = "apple", .sequence = 1, .type = ValueType::kValue});
  ASSERT_TRUE(table->Get(key, &value).ok());
  EXPECT_EQ(value, "red");
  key.clear();
  AppendInternalKey(
      &key, {.user_key = "cherry", .sequence = 3, .type = ValueType::kValue});
  ASSERT_TRUE(table->Get(key, &value).ok());
  EXPECT_EQ(value, "dark");
  key.clear();
  AppendInternalKey(
      &key, {.user_key = "cherry", .sequence = 2, .type = ValueType::kValue});
  EXPECT_TRUE(table->Get(key, &value).IsNotFound());
}

TEST_F(TableReaderTest, LookupKeyShorterThanTagIsNotFound) {
  MemTable memtable;
  memtable.Add(1, ValueType::kValue, "ab", "v");
  TableInfo info;
  ASSERT_TRUE(WriteMemTableToTable(memtable, {}, Path("t.sst"), &info).ok());
  std::unique_ptr<Table> table;
  ASSERT_TRUE(OpenTable(&table, CompareInternalKeys).ok());

  // A bare user key instead of an internal key, in a heap buffer of exactly
  // its size so ASan catches any read outside it.
  const std::vector<char> key = {'a', 'b'};
  std::string value;
  EXPECT_TRUE(table->Get(std::string_view(key.data(), key.size()), &value)
                  .IsNotFound());
}

TEST_F(TableReaderTest, SeekFindsFirstEntryAtOrAfterTarget) {
  const Entries entries = SortedEntries(200);
  BuildTable(entries, 64);  // many small blocks
  std::unique_ptr<Table> table;
  ASSERT_TRUE(OpenTable(&table).ok());

  std::string key;
  std::string value;
  for (size_t i = 0; i < entries.size(); ++i) {
    // Exact hit, and a target just after the previous key, both land on i.
    ASSERT_TRUE(table->Seek(entries[i].first, &key, &value).ok());
    EXPECT_EQ(key, entries[i].first);
    EXPECT_EQ(value, entries[i].second);
    if (i > 0) {
      ASSERT_TRUE(table->Seek(entries[i - 1].first + "x", &key, &value).ok());
      EXPECT_EQ(key, entries[i].first);
    }
  }
  ASSERT_TRUE(table->Seek("a", &key, &value).ok());  // before the first key
  EXPECT_EQ(key, entries.front().first);
  EXPECT_TRUE(table->Seek("zzz", &key, &value).IsNotFound());
}

// Flushes `memtable` and opens the result as an internal-key table.
class TableLookupTest : public TableReaderTest {
 protected:
  void Flush(const MemTable& memtable, size_t block_size) {
    TableBuilderOptions options;
    options.block_size = block_size;
    TableInfo info;
    ASSERT_TRUE(
        WriteMemTableToTable(memtable, options, Path("t.sst"), &info).ok());
    ASSERT_TRUE(OpenTable(&table_, CompareInternalKeys).ok());
  }

  // Run the lookup, expecting OK, and return the result.
  Table::LookupResult Lookup(std::string_view user_key, SequenceNumber sequence,
                             std::string* value) {
    Table::LookupResult result = Table::LookupResult::kNotFound;
    EXPECT_TRUE(table_->Get(user_key, sequence, value, &result).ok());
    return result;
  }

  std::unique_ptr<Table> table_;
};

TEST_F(TableLookupTest, FindsNewestVisibleVersion) {
  MemTable memtable;
  memtable.Add(1, ValueType::kValue, "apple", "red");
  memtable.Add(2, ValueType::kDeletion, "apple", "");
  memtable.Add(3, ValueType::kValue, "apple", "green");
  memtable.Add(4, ValueType::kValue, "cherry", "dark");
  Flush(memtable, 16);  // versions of "apple" span several blocks

  std::string value;
  EXPECT_EQ(Lookup("apple", kMaxSequenceNumber, &value),
            Table::LookupResult::kFound);
  EXPECT_EQ(value, "green");
  EXPECT_EQ(Lookup("apple", 3, &value), Table::LookupResult::kFound);
  EXPECT_EQ(value, "green");
  EXPECT_EQ(Lookup("apple", 2, &value), Table::LookupResult::kDeleted);
  EXPECT_EQ(Lookup("apple", 1, &value), Table::LookupResult::kFound);
  EXPECT_EQ(value, "red");
  EXPECT_EQ(Lookup("cherry", 4, &value), Table::LookupResult::kFound);
  EXPECT_EQ(value, "dark");
}

TEST_F(TableLookupTest, MissingOrNotYetVisibleKeysAreNotFound) {
  MemTable memtable;
  memtable.Add(1, ValueType::kValue, "apple", "red");
  memtable.Add(4, ValueType::kValue, "cherry", "dark");
  Flush(memtable, 16);

  std::string value = "untouched";
  EXPECT_EQ(Lookup("a", kMaxSequenceNumber, &value),  // before the first key
            Table::LookupResult::kNotFound);
  EXPECT_EQ(Lookup("app", kMaxSequenceNumber, &value),  // prefix of a key
            Table::LookupResult::kNotFound);
  EXPECT_EQ(Lookup("banana", kMaxSequenceNumber, &value),  // between keys
            Table::LookupResult::kNotFound);
  EXPECT_EQ(Lookup("zzz", kMaxSequenceNumber, &value),  // after the last key
            Table::LookupResult::kNotFound);
  // Only a newer version of "cherry" exists.
  EXPECT_EQ(Lookup("cherry", 3, &value), Table::LookupResult::kNotFound);
  EXPECT_EQ(value, "untouched");
}

TEST_F(TableLookupTest, ShortUserKeysAreFound) {
  MemTable memtable;
  memtable.Add(1, ValueType::kValue, "", "empty");
  memtable.Add(2, ValueType::kValue, "ab", "v");
  Flush(memtable, 4096);

  std::string value;
  EXPECT_EQ(Lookup("", kMaxSequenceNumber, &value),
            Table::LookupResult::kFound);
  EXPECT_EQ(value, "empty");
  EXPECT_EQ(Lookup("ab", kMaxSequenceNumber, &value),
            Table::LookupResult::kFound);
  EXPECT_EQ(value, "v");
}

TEST_F(TableLookupTest, MatchesMemTableOracle) {
  // NOLINTNEXTLINE(bugprone-random-generator-seed)
  std::mt19937 rng(20261103);
  MemTable memtable;
  constexpr int kKeys = 50;
  constexpr SequenceNumber kWrites = 2000;
  for (SequenceNumber seq = 1; seq <= kWrites; ++seq) {
    const std::string key = "k" + std::to_string(rng() % kKeys);
    if (rng() % 4 == 0) {
      memtable.Add(seq, ValueType::kDeletion, key, "");
    } else {
      memtable.Add(seq, ValueType::kValue, key, "v" + std::to_string(seq));
    }
  }
  Flush(memtable, 128);

  for (int k = 0; k <= kKeys; ++k) {  // k == kKeys is never written
    const std::string key = "k" + std::to_string(k);
    for (SequenceNumber seq = 0; seq <= kWrites + 1; seq += 7) {
      std::string want;
      std::string got;
      const MemTable::GetResult expected = memtable.Get(key, seq, &want);
      const Table::LookupResult actual = Lookup(key, seq, &got);
      switch (expected) {
        case MemTable::GetResult::kFound:
          ASSERT_EQ(actual, Table::LookupResult::kFound) << key << "@" << seq;
          ASSERT_EQ(got, want) << key << "@" << seq;
          break;
        case MemTable::GetResult::kDeleted:
          ASSERT_EQ(actual, Table::LookupResult::kDeleted) << key << "@" << seq;
          break;
        case MemTable::GetResult::kNotFound:
          ASSERT_EQ(actual, Table::LookupResult::kNotFound)
              << key << "@" << seq;
          break;
      }
    }
  }
}

TEST_F(TableLookupTest, CorruptBlockIsReported) {
  MemTable memtable;
  for (int i = 0; i < 100; ++i) {
    memtable.Add(static_cast<SequenceNumber>(i) + 1, ValueType::kValue,
                 "key" + std::to_string(1000 + i), "value");
  }
  Flush(memtable, 256);
  FlipByte(10);  // inside the first data block
  ASSERT_TRUE(OpenTable(&table_, CompareInternalKeys).ok());

  std::string value;
  Table::LookupResult result = Table::LookupResult::kNotFound;
  EXPECT_TRUE(table_->Get("key1000", kMaxSequenceNumber, &value, &result)
                  .IsCorruption());
}

TEST_F(TableReaderTest, FlippedDataByteIsCorruption) {
  BuildTable(SortedEntries(100), 256);
  FlipByte(10);  // inside the first data block
  std::unique_ptr<Table> table;
  ASSERT_TRUE(OpenTable(&table).ok());
  std::string value;
  EXPECT_TRUE(table->Get("key00000000", &value).IsCorruption());
  // Blocks that were not damaged still read fine.
  EXPECT_TRUE(table->Get("key00000099", &value).ok());
}

TEST_F(TableReaderTest, FlippedIndexByteFailsOpen) {
  BuildTable(SortedEntries(100), 256);
  const uint64_t size = std::filesystem::file_size(Path("t.sst"));
  FlipByte(size - Footer::kEncodedLength - 10);  // inside the index block
  std::unique_ptr<Table> table;
  EXPECT_TRUE(OpenTable(&table).IsCorruption());
}

TEST_F(TableReaderTest, BadFooterFailsOpen) {
  BuildTable(SortedEntries(10), 256);
  const uint64_t size = std::filesystem::file_size(Path("t.sst"));
  FlipByte(size - 1);  // magic number
  std::unique_ptr<Table> table;
  EXPECT_TRUE(OpenTable(&table).IsCorruption());
}

TEST_F(TableReaderTest, TruncatedFileFailsOpen) {
  BuildTable(SortedEntries(100), 256);
  const uint64_t size = std::filesystem::file_size(Path("t.sst"));
  std::unique_ptr<Table> table;
  for (const uint64_t cut : {uint64_t{0}, uint64_t{10}, size / 2, size - 1}) {
    std::filesystem::resize_file(Path("t.sst"), cut);
    EXPECT_TRUE(OpenTable(&table).IsCorruption()) << cut;
  }
}

}  // namespace
}  // namespace sediment
