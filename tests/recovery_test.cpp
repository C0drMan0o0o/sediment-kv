#include "sediment/recovery.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include "sediment/file.h"
#include "sediment/wal.h"

namespace sediment {
namespace {

class RecoveryTest : public testing::Test {
 protected:
  void SetUp() override {
    const auto* info = testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() /
           (std::string("sediment_recovery_test_") + info->name());
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }

  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::string Path() const { return (dir_ / "log").string(); }

  // Append raw payloads as WAL records.
  void WriteRecords(const std::vector<std::string>& payloads) {
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(NewWritableFile(Path(), &file).ok());
    wal::Writer writer(std::move(file));
    for (const auto& p : payloads) {
      ASSERT_TRUE(writer.AddRecord(p).ok());
    }
    ASSERT_TRUE(writer.Close().ok());
  }

  static std::string Entry(SequenceNumber seq, ValueType type,
                           std::string_view key, std::string_view value) {
    std::string e;
    EncodeLogEntry(&e, seq, type, key, value);
    return e;
  }

  std::filesystem::path dir_;
};

TEST_F(RecoveryTest, ReplaysPutsAndDeletesIntoMemtable) {
  WriteRecords({Entry(1, ValueType::kValue, "a", "one"),
                Entry(2, ValueType::kValue, "b", "two"),
                Entry(3, ValueType::kDeletion, "a", ""),
                Entry(4, ValueType::kValue, "b", "three")});

  MemTable mem;
  RecoveryStats stats;
  ASSERT_TRUE(RecoverFromLog(Path(), &mem, &stats).ok());

  EXPECT_EQ(stats.records, 4u);
  EXPECT_EQ(stats.last_sequence, 4u);
  EXPECT_EQ(stats.dropped_bytes, 0u);
  EXPECT_EQ(stats.stop_reason, wal::StopReason::kNone);

  std::string value;
  EXPECT_EQ(mem.Get("a", 4, &value), MemTable::GetResult::kDeleted);
  EXPECT_EQ(mem.Get("a", 2, &value), MemTable::GetResult::kFound);
  EXPECT_EQ(value, "one");
  EXPECT_EQ(mem.Get("b", 4, &value), MemTable::GetResult::kFound);
  EXPECT_EQ(value, "three");
  EXPECT_EQ(mem.Get("b", 3, &value), MemTable::GetResult::kFound);
  EXPECT_EQ(value, "two");
}

TEST_F(RecoveryTest, EmptyLogRecoversNothing) {
  WriteRecords({});
  MemTable mem;
  RecoveryStats stats;
  ASSERT_TRUE(RecoverFromLog(Path(), &mem, &stats).ok());
  EXPECT_EQ(stats.records, 0u);
  EXPECT_EQ(stats.last_sequence, 0u);
}

TEST_F(RecoveryTest, MissingLogIsAnError) {
  MemTable mem;
  RecoveryStats stats;
  EXPECT_FALSE(RecoverFromLog(Path(), &mem, &stats).ok());
}

TEST_F(RecoveryTest, TornTailKeepsEarlierWrites) {
  WriteRecords({Entry(1, ValueType::kValue, "a", "one"),
                Entry(2, ValueType::kValue, "b", "two")});
  const auto size = std::filesystem::file_size(Path());
  std::filesystem::resize_file(Path(), size - 3);

  MemTable mem;
  RecoveryStats stats;
  ASSERT_TRUE(RecoverFromLog(Path(), &mem, &stats).ok());

  EXPECT_EQ(stats.records, 1u);
  EXPECT_EQ(stats.last_sequence, 1u);
  EXPECT_GT(stats.dropped_bytes, 0u);
  EXPECT_EQ(stats.stop_reason, wal::StopReason::kTruncatedPayload);

  std::string value;
  EXPECT_EQ(mem.Get("a", 2, &value), MemTable::GetResult::kFound);
  EXPECT_EQ(mem.Get("b", 2, &value), MemTable::GetResult::kNotFound);
}

TEST_F(RecoveryTest, DamageBeforeLaterWritesIsCorruption) {
  WriteRecords({Entry(1, ValueType::kValue, "a", "one"),
                Entry(2, ValueType::kValue, "b", "two"),
                Entry(3, ValueType::kValue, "c", "three")});
  // Flip the last byte of the second frame (inside its payload).
  const size_t first_frame =
      wal::kHeaderSize + Entry(1, ValueType::kValue, "a", "one").size();
  const size_t second_frame =
      wal::kHeaderSize + Entry(2, ValueType::kValue, "b", "two").size();
  std::fstream f(Path(), std::ios::in | std::ios::out | std::ios::binary);
  f.seekp(static_cast<std::streamoff>(first_frame + second_frame - 1));
  f.put('\xee');
  f.close();

  MemTable mem;
  RecoveryStats stats;
  EXPECT_TRUE(RecoverFromLog(Path(), &mem, &stats).IsCorruption());
  EXPECT_EQ(stats.records, 1u);
  EXPECT_EQ(stats.stop_reason, wal::StopReason::kBadChecksum);
}

TEST_F(RecoveryTest, IntactFrameWithGarbageEntryIsCorruption) {
  WriteRecords({Entry(1, ValueType::kValue, "a", "one"), "xyz"});
  MemTable mem;
  RecoveryStats stats;
  EXPECT_TRUE(RecoverFromLog(Path(), &mem, &stats).IsCorruption());
}

TEST_F(RecoveryTest, UnknownEntryTypeIsCorruption) {
  std::string bad = Entry(1, ValueType::kValue, "a", "one");
  bad[8] = 7;  // type byte follows the 8-byte sequence
  WriteRecords({bad});
  MemTable mem;
  RecoveryStats stats;
  EXPECT_TRUE(RecoverFromLog(Path(), &mem, &stats).IsCorruption());
}

TEST_F(RecoveryTest, TrailingBytesInEntryAreCorruption) {
  WriteRecords({Entry(1, ValueType::kValue, "a", "one") + "x"});
  MemTable mem;
  RecoveryStats stats;
  EXPECT_TRUE(RecoverFromLog(Path(), &mem, &stats).IsCorruption());
}

TEST_F(RecoveryTest, HandlesEmptyKeyAndLargeValue) {
  const std::string big(300000, 'v');
  WriteRecords({Entry(1, ValueType::kValue, "", big)});
  MemTable mem;
  RecoveryStats stats;
  ASSERT_TRUE(RecoverFromLog(Path(), &mem, &stats).ok());
  std::string value;
  EXPECT_EQ(mem.Get("", 1, &value), MemTable::GetResult::kFound);
  EXPECT_EQ(value, big);
}

#ifndef NDEBUG
// Lengths are stored as varint32. Each view below claims more than 4 GiB over
// a 1-byte buffer, which is safe only because the length check comes before
// any byte is read.
TEST(RecoveryDeathTest, LogEntryKeyTooLongForVarint32Asserts) {
  const char byte = 'k';
  std::string entry;
  EXPECT_DEATH(
      EncodeLogEntry(&entry, 1, ValueType::kValue,
                     std::string_view(&byte, size_t{UINT32_MAX} + 1), "v"),
      "ssertion");
}

TEST(RecoveryDeathTest, LogEntryValueTooLongForVarint32Asserts) {
  const char byte = 'v';
  std::string entry;
  EXPECT_DEATH(EncodeLogEntry(&entry, 1, ValueType::kValue, "k",
                              std::string_view(&byte, size_t{UINT32_MAX} + 1)),
               "ssertion");
}
#endif

}  // namespace
}  // namespace sediment
