#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "sediment/file.h"
#include "sediment/wal.h"

namespace sediment::wal {
namespace {

class WalReaderTest : public testing::Test {
 protected:
  void SetUp() override {
    const auto* info = testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() /
           (std::string("sediment_wal_reader_test_") + info->name());
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }

  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::string Path() const { return (dir_ / "log").string(); }

  // Write `payloads` through Writer and return the resulting file bytes.
  std::string WriteLog(const std::vector<std::string>& payloads) {
    std::unique_ptr<WritableFile> file;
    EXPECT_TRUE(NewWritableFile(Path(), &file).ok());
    Writer writer(std::move(file));
    for (const auto& p : payloads) {
      EXPECT_TRUE(writer.AddRecord(p).ok());
    }
    EXPECT_TRUE(writer.Close().ok());
    return ReadFile();
  }

  std::string ReadFile() const {
    std::ifstream in(Path(), std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
  }

  void Overwrite(const std::string& bytes) const {
    std::ofstream out(Path(), std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  struct ReadResult {
    std::vector<std::string> records;
    StopReason reason;
    uint64_t dropped;
    Status status;
  };

  ReadResult ReadAll() const {
    std::unique_ptr<SequentialFile> file;
    EXPECT_TRUE(NewSequentialFile(Path(), &file).ok());
    Reader reader(std::move(file));
    ReadResult result;
    std::string record;
    while (reader.ReadRecord(&record)) {
      result.records.push_back(record);
    }
    result.reason = reader.stop_reason();
    result.dropped = reader.dropped_bytes();
    result.status = reader.status();
    return result;
  }

  static std::vector<std::string> SamplePayloads() {
    return {"alpha", "", "a-somewhat-longer-payload", std::string(300, 'z'),
            "omega"};
  }

  static uint64_t FrameSize(const std::string& payload) {
    return kHeaderSize + payload.size();
  }

  std::filesystem::path dir_;
};

TEST_F(WalReaderTest, EmptyFileIsACleanEnd) {
  WriteLog({});
  const ReadResult r = ReadAll();
  EXPECT_TRUE(r.records.empty());
  EXPECT_EQ(r.reason, StopReason::kNone);
  EXPECT_EQ(r.dropped, 0u);
  EXPECT_TRUE(r.status.ok());
}

TEST_F(WalReaderTest, RoundTripsThousandRecords) {
  std::vector<std::string> payloads;
  payloads.reserve(1000);
  for (int i = 0; i < 1000; ++i) {
    payloads.push_back("record-" + std::to_string(i));
  }
  WriteLog(payloads);

  const ReadResult r = ReadAll();
  EXPECT_EQ(r.records, payloads);
  EXPECT_EQ(r.reason, StopReason::kNone);
  EXPECT_EQ(r.dropped, 0u);
}

TEST_F(WalReaderTest, RoundTripsLargeAndBinaryPayloads) {
  const std::vector<std::string> payloads = {
      std::string("a\0b\xff\x01", 5), std::string(size_t{1024} * 1024, 'x'),
      "end"};
  WriteLog(payloads);
  EXPECT_EQ(ReadAll().records, payloads);
}

TEST_F(WalReaderTest, EveryTruncationPointReturnsACleanPrefix) {
  const auto payloads = SamplePayloads();
  const std::string full = WriteLog(payloads);

  for (size_t cut = 0; cut <= full.size(); ++cut) {
    Overwrite(full.substr(0, cut));
    const ReadResult r = ReadAll();

    // Count the frames that fit entirely within `cut` bytes.
    size_t whole = 0;
    uint64_t end = 0;
    while (whole < payloads.size() && end + FrameSize(payloads[whole]) <= cut) {
      end += FrameSize(payloads[whole++]);
    }
    ASSERT_EQ(r.records.size(), whole) << "cut=" << cut;
    for (size_t i = 0; i < whole; ++i) {
      EXPECT_EQ(r.records[i], payloads[i]) << "cut=" << cut;
    }
    EXPECT_EQ(r.dropped, cut - end) << "cut=" << cut;
    if (cut == end) {
      EXPECT_EQ(r.reason, StopReason::kNone) << "cut=" << cut;
    } else if (cut - end < kHeaderSize) {
      EXPECT_EQ(r.reason, StopReason::kTruncatedHeader) << "cut=" << cut;
    } else {
      EXPECT_EQ(r.reason, StopReason::kTruncatedPayload) << "cut=" << cut;
    }
    EXPECT_TRUE(r.status.ok());
  }
}

TEST_F(WalReaderTest, FlippedBytesStopAtOrBeforeTheDamage) {
  const auto payloads = SamplePayloads();
  const std::string full = WriteLog(payloads);
  // NOLINTNEXTLINE(bugprone-random-generator-seed)
  std::mt19937 rng(12345);

  for (int iter = 0; iter < 2000; ++iter) {
    std::string damaged = full;
    const size_t pos = rng() % damaged.size();
    const auto mask = static_cast<uint8_t>(1 + rng() % 255);
    damaged[pos] = static_cast<char>(static_cast<uint8_t>(damaged[pos]) ^ mask);
    Overwrite(damaged);
    const ReadResult r = ReadAll();

    // Index of the frame holding the flipped byte.
    size_t damaged_frame = 0;
    uint64_t start = 0;
    while (pos >= start + FrameSize(payloads[damaged_frame])) {
      start += FrameSize(payloads[damaged_frame++]);
    }

    ASSERT_LE(r.records.size(), damaged_frame) << "pos=" << pos;
    uint64_t returned_bytes = 0;
    for (size_t i = 0; i < r.records.size(); ++i) {
      EXPECT_EQ(r.records[i], payloads[i]) << "pos=" << pos;
      returned_bytes += FrameSize(r.records[i]);
    }
    // Every byte is either returned or reported as dropped.
    EXPECT_EQ(returned_bytes + r.dropped, full.size()) << "pos=" << pos;
    EXPECT_NE(r.reason, StopReason::kNone) << "pos=" << pos;
    EXPECT_TRUE(r.status.ok());
  }
}

TEST_F(WalReaderTest, ZeroFilledTailIsDropped) {
  const auto payloads = SamplePayloads();
  std::string bytes = WriteLog(payloads);
  bytes.append(64, '\0');
  Overwrite(bytes);

  const ReadResult r = ReadAll();
  EXPECT_EQ(r.records, payloads);
  EXPECT_EQ(r.reason, StopReason::kBadType);
  EXPECT_EQ(r.dropped, 64u);
}

TEST_F(WalReaderTest, HugeCorruptLengthDoesNotOverAllocate) {
  std::string bytes = WriteLog({"first", "second"});
  // Overwrite the second frame's length with 0xFFFFFFFF.
  const size_t length_pos = FrameSize("first") + 4;
  for (size_t i = 0; i < 4; ++i) {
    bytes[length_pos + i] = '\xff';
  }
  Overwrite(bytes);

  const ReadResult r = ReadAll();
  ASSERT_EQ(r.records.size(), 1u);
  EXPECT_EQ(r.records[0], "first");
  EXPECT_EQ(r.reason, StopReason::kTruncatedPayload);
  EXPECT_EQ(r.dropped, FrameSize("second"));
}

TEST_F(WalReaderTest, StaysStoppedAfterDamage) {
  std::string bytes = WriteLog({"one", "two", "three"});
  // Corrupt the first payload byte of "two".
  char& target = bytes[FrameSize("one") + kHeaderSize];
  target = static_cast<char>(static_cast<uint8_t>(target) ^ 0x01U);
  Overwrite(bytes);

  std::unique_ptr<SequentialFile> file;
  ASSERT_TRUE(NewSequentialFile(Path(), &file).ok());
  Reader reader(std::move(file));
  std::string record;
  ASSERT_TRUE(reader.ReadRecord(&record));
  EXPECT_EQ(record, "one");
  EXPECT_FALSE(reader.ReadRecord(&record));
  EXPECT_EQ(reader.stop_reason(), StopReason::kBadChecksum);
  EXPECT_FALSE(reader.ReadRecord(&record));
  EXPECT_EQ(reader.dropped_bytes(), FrameSize("two") + FrameSize("three"));
}

}  // namespace
}  // namespace sediment::wal
