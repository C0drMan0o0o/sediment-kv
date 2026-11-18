#include <gtest/gtest.h>

#include <algorithm>
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
#include "sediment/table_reader.h"

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

// Every entry from the iterator's current position to the end.
Entries ScanFromHere(Table::Iterator* it) {
  Entries out;
  for (; it->Valid(); it->Next()) {
    out.emplace_back(it->key(), it->value());
  }
  return out;
}

class TableIteratorTest : public testing::Test {
 protected:
  void SetUp() override {
    const auto* info = testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() /
           (std::string("sediment_table_iterator_test_") + info->name());
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }

  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::string Path() const { return (dir_ / "t.sst").string(); }

  void BuildTable(const Entries& entries, size_t block_size) {
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(NewWritableFile(Path(), &file).ok());
    TableBuilderOptions options;
    options.block_size = block_size;
    TableBuilder builder(options, file.get());
    for (const auto& [k, v] : entries) {
      builder.Add(k, v);
    }
    ASSERT_TRUE(builder.Finish().ok());
    ASSERT_TRUE(file->Close().ok());
  }

  void OpenTable(KeyComparator comparator = BytewiseCompare) {
    std::unique_ptr<RandomAccessFile> file;
    ASSERT_TRUE(NewRandomAccessFile(Path(), &file).ok());
    ASSERT_TRUE(Table::Open(std::move(file), std::filesystem::file_size(Path()),
                            comparator, &table_)
                    .ok());
  }

  void FlipByte(uint64_t offset) {
    std::fstream f(Path(), std::ios::in | std::ios::out | std::ios::binary);
    f.seekg(static_cast<std::streamoff>(offset));
    char c = 0;
    f.get(c);
    const auto flipped = static_cast<unsigned char>(c) ^ 0x01U;
    f.seekp(static_cast<std::streamoff>(offset));
    f.put(static_cast<char>(flipped));
  }

  std::filesystem::path dir_;
  std::unique_ptr<Table> table_;
};

TEST_F(TableIteratorTest, ScanReturnsEveryEntryInOrderAcrossBlocks) {
  const Entries entries = SortedEntries(2000);
  BuildTable(entries, 256);
  OpenTable();

  Table::Iterator it = table_->NewIterator();
  it.SeekToFirst();
  EXPECT_EQ(ScanFromHere(&it), entries);
  EXPECT_TRUE(it.status().ok());
}

TEST_F(TableIteratorTest, OneEntryPerBlock) {
  const Entries entries = SortedEntries(20);
  BuildTable(entries, 1);
  OpenTable();

  Table::Iterator it = table_->NewIterator();
  it.SeekToFirst();
  EXPECT_EQ(ScanFromHere(&it), entries);
  EXPECT_TRUE(it.status().ok());
}

TEST_F(TableIteratorTest, EmptyTableIsNeverValid) {
  BuildTable({}, 4096);
  OpenTable();

  Table::Iterator it = table_->NewIterator();
  EXPECT_FALSE(it.Valid());
  it.SeekToFirst();
  EXPECT_FALSE(it.Valid());
  it.Seek("k");
  EXPECT_FALSE(it.Valid());
  EXPECT_TRUE(it.status().ok());
}

TEST_F(TableIteratorTest, SeekMatchesSortedVectorOracle) {
  const Entries entries = SortedEntries(500);
  BuildTable(entries, 64);  // many small blocks
  OpenTable();

  std::vector<std::string> targets = {"", "a", "key", "zzz"};
  for (const auto& [k, v] : entries) {
    targets.push_back(k);
    targets.push_back(k + "x");  // between this key and the next
  }
  // NOLINTNEXTLINE(bugprone-random-generator-seed)
  std::mt19937 rng(20261113);
  for (int i = 0; i < 200; ++i) {
    std::array<char, 32> key{};
    std::snprintf(key.data(), key.size(), "key%08u",
                  static_cast<unsigned>(rng() % 600));
    targets.emplace_back(key.data());
  }

  Table::Iterator it = table_->NewIterator();
  for (const std::string& target : targets) {
    const auto want = std::ranges::lower_bound(entries, target, {},
                                               &Entries::value_type::first);
    it.Seek(target);
    // Check the landing entry and a few after it, which may cross blocks.
    auto expected = want;
    for (int step = 0; step < 5 && expected != entries.end(); ++step) {
      ASSERT_TRUE(it.Valid()) << target;
      ASSERT_EQ(it.key(), expected->first) << target;
      ASSERT_EQ(it.value(), expected->second) << target;
      it.Next();
      ++expected;
    }
    if (expected == entries.end()) {
      EXPECT_FALSE(it.Valid()) << target;
    }
    EXPECT_TRUE(it.status().ok()) << target;
  }
}

TEST_F(TableIteratorTest, SeekThenScanReachesTheEnd) {
  const Entries entries = SortedEntries(300);
  BuildTable(entries, 128);
  OpenTable();

  Table::Iterator it = table_->NewIterator();
  it.Seek(entries[150].first);
  EXPECT_EQ(ScanFromHere(&it), Entries(entries.begin() + 150, entries.end()));
  // The iterator can be repositioned after reaching the end.
  it.SeekToFirst();
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), entries.front().first);
}

TEST_F(TableIteratorTest, MovedIteratorKeepsItsPosition) {
  const Entries entries = SortedEntries(100);
  BuildTable(entries, 64);
  OpenTable();

  Table::Iterator it = table_->NewIterator();
  it.Seek(entries[10].first);
  Table::Iterator moved = std::move(it);
  EXPECT_EQ(ScanFromHere(&moved), Entries(entries.begin() + 10, entries.end()));
}

TEST_F(TableIteratorTest, MemTableRoundTripScanMatchesExactly) {
  // NOLINTNEXTLINE(bugprone-random-generator-seed)
  std::mt19937 rng(20261114);
  MemTable memtable;
  for (SequenceNumber seq = 1; seq <= 5000; ++seq) {
    const std::string key = "k" + std::to_string(rng() % 300);
    if (rng() % 4 == 0) {
      memtable.Add(seq, ValueType::kDeletion, key, "");
    } else {
      memtable.Add(seq, ValueType::kValue, key, "v" + std::to_string(seq));
    }
  }
  Entries expected;
  MemTable::Iterator mem_it(&memtable);
  for (mem_it.SeekToFirst(); mem_it.Valid(); mem_it.Next()) {
    expected.emplace_back(mem_it.key(), mem_it.value());
  }

  TableBuilderOptions options;
  options.block_size = 256;
  TableInfo info;
  ASSERT_TRUE(WriteMemTableToTable(memtable, options, Path(), &info).ok());
  OpenTable(CompareInternalKeys);

  Table::Iterator it = table_->NewIterator();
  it.SeekToFirst();
  const Entries scanned = ScanFromHere(&it);
  EXPECT_TRUE(it.status().ok());
  EXPECT_EQ(scanned.size(), info.num_entries);
  EXPECT_EQ(scanned, expected);
}

TEST_F(TableIteratorTest, CorruptBlockStopsScanWithCorruption) {
  const Entries entries = SortedEntries(200);
  BuildTable(entries, 256);
  const uint64_t size = std::filesystem::file_size(Path());
  FlipByte(size / 3);  // inside a data block part-way through the file
  OpenTable();

  Table::Iterator it = table_->NewIterator();
  it.SeekToFirst();
  const Entries scanned = ScanFromHere(&it);
  EXPECT_TRUE(it.status().IsCorruption());
  // Blocks before the damaged one were returned intact.
  ASSERT_GT(scanned.size(), 0U);
  ASSERT_LT(scanned.size(), entries.size());
  EXPECT_EQ(scanned, Entries(entries.begin(),
                             entries.begin() +
                                 static_cast<std::ptrdiff_t>(scanned.size())));
}

}  // namespace
}  // namespace sediment
