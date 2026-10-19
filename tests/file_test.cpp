#include "sediment/file.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace sediment {
namespace {

class FileTest : public testing::Test {
 protected:
  void SetUp() override {
    const auto* info = testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() /
           (std::string("sediment_file_test_") + info->name());
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }

  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::string Path(const std::string& name) const {
    return (dir_ / name).string();
  }

  // Write `data` to `name`, syncing and closing it.
  void WriteFile(const std::string& name, std::string_view data) {
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(NewWritableFile(Path(name), &file).ok());
    ASSERT_TRUE(file->Append(data).ok());
    ASSERT_TRUE(file->Sync().ok());
    ASSERT_TRUE(file->Close().ok());
  }

  std::filesystem::path dir_;
};

TEST_F(FileTest, WriteSyncReopenAndReadBackSequentially) {
  WriteFile("a", "hello, world");

  std::unique_ptr<SequentialFile> file;
  ASSERT_TRUE(NewSequentialFile(Path("a"), &file).ok());
  char scratch[64];
  std::string_view result;
  ASSERT_TRUE(file->Read(5, scratch, &result).ok());
  EXPECT_EQ(result, "hello");
  ASSERT_TRUE(file->Read(sizeof(scratch), scratch, &result).ok());
  EXPECT_EQ(result, ", world");
  ASSERT_TRUE(file->Read(sizeof(scratch), scratch, &result).ok());
  EXPECT_TRUE(result.empty());
}

TEST_F(FileTest, MultipleAppendsAreConcatenated) {
  std::unique_ptr<WritableFile> file;
  ASSERT_TRUE(NewWritableFile(Path("a"), &file).ok());
  ASSERT_TRUE(file->Append("abc").ok());
  ASSERT_TRUE(file->Append("").ok());
  ASSERT_TRUE(file->Append("def").ok());
  ASSERT_TRUE(file->Close().ok());

  EXPECT_EQ(std::filesystem::file_size(Path("a")), 6U);
  std::unique_ptr<RandomAccessFile> reader;
  ASSERT_TRUE(NewRandomAccessFile(Path("a"), &reader).ok());
  char scratch[8];
  std::string_view result;
  ASSERT_TRUE(reader->Read(0, 6, scratch, &result).ok());
  EXPECT_EQ(result, "abcdef");
}

TEST_F(FileTest, LargeAppendRoundTrips) {
  std::string data(3 << 20, '\0');
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<char>(i * 31 + (i >> 8));
  }
  WriteFile("big", data);

  std::unique_ptr<RandomAccessFile> file;
  ASSERT_TRUE(NewRandomAccessFile(Path("big"), &file).ok());
  std::string scratch(data.size(), '\0');
  std::string_view result;
  ASSERT_TRUE(file->Read(0, data.size(), scratch.data(), &result).ok());
  EXPECT_EQ(result, data);
}

TEST_F(FileTest, NewWritableFileTruncatesExistingFile) {
  WriteFile("a", "old contents that are long");
  WriteFile("a", "new");
  EXPECT_EQ(std::filesystem::file_size(Path("a")), 3U);
}

TEST_F(FileTest, SequentialSkip) {
  WriteFile("a", "0123456789");
  std::unique_ptr<SequentialFile> file;
  ASSERT_TRUE(NewSequentialFile(Path("a"), &file).ok());
  ASSERT_TRUE(file->Skip(4).ok());
  char scratch[16];
  std::string_view result;
  ASSERT_TRUE(file->Read(3, scratch, &result).ok());
  EXPECT_EQ(result, "456");
  ASSERT_TRUE(file->Skip(100).ok());
  ASSERT_TRUE(file->Read(3, scratch, &result).ok());
  EXPECT_TRUE(result.empty());
}

TEST_F(FileTest, RandomAccessReadsAtOffsets) {
  WriteFile("a", "0123456789");
  std::unique_ptr<RandomAccessFile> file;
  ASSERT_TRUE(NewRandomAccessFile(Path("a"), &file).ok());
  char scratch[16];
  std::string_view result;

  ASSERT_TRUE(file->Read(7, 2, scratch, &result).ok());
  EXPECT_EQ(result, "78");
  ASSERT_TRUE(file->Read(0, 3, scratch, &result).ok());
  EXPECT_EQ(result, "012");
}

TEST_F(FileTest, RandomAccessReadPastEndIsShortNotAnError) {
  WriteFile("a", "0123456789");
  std::unique_ptr<RandomAccessFile> file;
  ASSERT_TRUE(NewRandomAccessFile(Path("a"), &file).ok());
  char scratch[16];
  std::string_view result;

  ASSERT_TRUE(file->Read(8, 10, scratch, &result).ok());
  EXPECT_EQ(result, "89");
  ASSERT_TRUE(file->Read(10, 4, scratch, &result).ok());
  EXPECT_TRUE(result.empty());
  ASSERT_TRUE(file->Read(1000, 4, scratch, &result).ok());
  EXPECT_TRUE(result.empty());
}

TEST_F(FileTest, OpeningMissingFileForReadingReturnsIOError) {
  std::unique_ptr<SequentialFile> seq;
  const Status s1 = NewSequentialFile(Path("missing"), &seq);
  EXPECT_TRUE(s1.IsIOError());
  EXPECT_EQ(seq, nullptr);
  EXPECT_NE(s1.message().find("missing"), std::string_view::npos);

  std::unique_ptr<RandomAccessFile> rnd;
  EXPECT_TRUE(NewRandomAccessFile(Path("missing"), &rnd).IsIOError());
  EXPECT_EQ(rnd, nullptr);
}

TEST_F(FileTest, CreatingFileInMissingDirectoryReturnsIOError) {
  std::unique_ptr<WritableFile> file;
  EXPECT_TRUE(NewWritableFile(Path("no/such/dir/a"), &file).IsIOError());
  EXPECT_EQ(file, nullptr);
}

TEST_F(FileTest, UseAfterCloseReturnsInvalidArgumentAndCloseIsIdempotent) {
  std::unique_ptr<WritableFile> file;
  ASSERT_TRUE(NewWritableFile(Path("a"), &file).ok());
  ASSERT_TRUE(file->Close().ok());
  EXPECT_TRUE(file->Close().ok());
  EXPECT_TRUE(file->Append("x").IsInvalidArgument());
  EXPECT_TRUE(file->Sync().IsInvalidArgument());
}

}  // namespace
}  // namespace sediment
