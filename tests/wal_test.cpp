#include "sediment/wal.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

#include "sediment/coding.h"
#include "sediment/crc32c.h"
#include "sediment/file.h"

namespace sediment::wal {
namespace {

class WalWriterTest : public testing::Test {
 protected:
  void SetUp() override {
    const auto* info = testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() /
           (std::string("sediment_wal_test_") + info->name());
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }

  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::string Path() const { return (dir_ / "log").string(); }

  std::unique_ptr<Writer> NewWriter() {
    std::unique_ptr<WritableFile> file;
    EXPECT_TRUE(NewWritableFile(Path(), &file).ok());
    return std::make_unique<Writer>(std::move(file));
  }

  std::string ReadAll() const {
    std::ifstream in(Path(), std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
  }

  // Check that `bytes` at `*pos` holds a valid frame for `payload`.
  static void ExpectFrame(const std::string& bytes, size_t* pos,
                          std::string_view payload) {
    ASSERT_GE(bytes.size(), *pos + kHeaderSize + payload.size());
    const char* p = bytes.data() + *pos;
    uint32_t length = 0;
    std::string_view len_view(p + 4, 4);
    ASSERT_TRUE(GetFixed32(&len_view, &length).ok());
    EXPECT_EQ(length, payload.size());
    EXPECT_EQ(static_cast<uint8_t>(p[8]),
              static_cast<uint8_t>(RecordType::kFull));
    EXPECT_EQ(std::string_view(p + kHeaderSize, length), payload);

    uint32_t stored = 0;
    std::string_view crc_view(p, 4);
    ASSERT_TRUE(GetFixed32(&crc_view, &stored).ok());
    EXPECT_EQ(crc32c::Unmask(stored), crc32c::Value(p + 8, 1 + length));
    *pos += kHeaderSize + length;
  }

  std::filesystem::path dir_;
};

TEST_F(WalWriterTest, WritesFrameLayout) {
  auto writer = NewWriter();
  ASSERT_TRUE(writer->AddRecord("hello").ok());
  ASSERT_TRUE(writer->Close().ok());

  const std::string bytes = ReadAll();
  EXPECT_EQ(bytes.size(), kHeaderSize + 5);
  size_t pos = 0;
  ExpectFrame(bytes, &pos, "hello");
}

TEST_F(WalWriterTest, EmptyPayloadIsAValidRecord) {
  auto writer = NewWriter();
  ASSERT_TRUE(writer->AddRecord("").ok());
  ASSERT_TRUE(writer->Close().ok());

  const std::string bytes = ReadAll();
  EXPECT_EQ(bytes.size(), kHeaderSize);
  size_t pos = 0;
  ExpectFrame(bytes, &pos, "");
}

TEST_F(WalWriterTest, ThousandRecordsAreContiguous) {
  auto writer = NewWriter();
  std::string expected_payloads[1000];
  for (int i = 0; i < 1000; ++i) {
    expected_payloads[i] = "record-" + std::to_string(i);
    ASSERT_TRUE(writer->AddRecord(expected_payloads[i], i % 100 == 0).ok());
  }
  ASSERT_TRUE(writer->Close().ok());

  const std::string bytes = ReadAll();
  size_t pos = 0;
  for (const auto& payload : expected_payloads) {
    ExpectFrame(bytes, &pos, payload);
  }
  EXPECT_EQ(pos, bytes.size());
}

TEST_F(WalWriterTest, LargeRecordIsOneFrame) {
  const std::string big(1 << 20, 'x');
  auto writer = NewWriter();
  ASSERT_TRUE(writer->AddRecord("small").ok());
  ASSERT_TRUE(writer->AddRecord(big, true).ok());
  ASSERT_TRUE(writer->AddRecord("after").ok());
  ASSERT_TRUE(writer->Close().ok());

  const std::string bytes = ReadAll();
  size_t pos = 0;
  ExpectFrame(bytes, &pos, "small");
  ExpectFrame(bytes, &pos, big);
  ExpectFrame(bytes, &pos, "after");
  EXPECT_EQ(pos, bytes.size());
}

TEST_F(WalWriterTest, BinaryPayloadRoundTrips) {
  const std::string payload("a\0b\xff\x01", 5);
  auto writer = NewWriter();
  ASSERT_TRUE(writer->AddRecord(payload).ok());
  ASSERT_TRUE(writer->Close().ok());

  const std::string bytes = ReadAll();
  size_t pos = 0;
  ExpectFrame(bytes, &pos, payload);
}

TEST_F(WalWriterTest, FailedAppendIsSticky) {
  auto writer = NewWriter();
  ASSERT_TRUE(writer->AddRecord("ok").ok());
  ASSERT_TRUE(writer->Close().ok());

  // The file is closed, so the next append fails and poisons the writer.
  const Status first = writer->AddRecord("fails");
  EXPECT_FALSE(first.ok());
  EXPECT_EQ(writer->AddRecord("again"), first);
}

}  // namespace
}  // namespace sediment::wal
