#include "sediment/filename.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace sediment {
namespace {

TEST(FileNameTest, NamesAreZeroPaddedToSixDigits) {
  EXPECT_EQ(WalFileName("db", 123), "db/000123.wal");
  EXPECT_EQ(TableFileName("db", 45), "db/000045.sst");
  EXPECT_EQ(TableFileName("db", 1234567), "db/1234567.sst");
  EXPECT_EQ(LockFileName("/tmp/db"), "/tmp/db/LOCK");
}

TEST(FileNameTest, ParseRoundTripsGeneratedNames) {
  for (const uint64_t n : {uint64_t{0}, uint64_t{1}, uint64_t{999999},
                           uint64_t{1000000}, UINT64_MAX}) {
    uint64_t number = 0;
    FileType type{};
    ASSERT_TRUE(ParseFileName(WalFileName(".", n).substr(2), &number, &type));
    EXPECT_EQ(number, n);
    EXPECT_EQ(type, FileType::kWal);
    ASSERT_TRUE(ParseFileName(TableFileName(".", n).substr(2), &number, &type));
    EXPECT_EQ(number, n);
    EXPECT_EQ(type, FileType::kTable);
  }
}

TEST(FileNameTest, ParseRejectsOtherNames) {
  for (const char* name : {"LOCK", "", ".wal", "x00001.wal", "000001.wal.tmp",
                           "000001.log", "-00001.sst", "000001",
                           "18446744073709551616.wal" /* UINT64_MAX+1 */}) {
    uint64_t number = 7;
    FileType type = FileType::kTable;
    EXPECT_FALSE(ParseFileName(name, &number, &type)) << name;
    EXPECT_EQ(number, 7U) << name;
  }
}

}  // namespace
}  // namespace sediment
