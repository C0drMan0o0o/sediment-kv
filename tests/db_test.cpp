#include "sediment/db.h"

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "sediment/filename.h"

namespace sediment {
namespace {

class DBTest : public testing::Test {
 protected:
  void SetUp() override {
    const auto* info = testing::UnitTest::GetInstance()->current_test_info();
    root_ = std::filesystem::temp_directory_path() /
            (std::string("sediment_db_test_") + info->name());
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
    path_ = (root_ / "db").string();
  }

  void TearDown() override { std::filesystem::remove_all(root_); }

  std::vector<std::string> Children() const {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(path_)) {
      names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
  }

  std::filesystem::path root_;
  std::string path_;
};

TEST_F(DBTest, OpenCreatesDirectoryWithLockAndFirstWal) {
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(Options(), path_, &db).ok());
  ASSERT_NE(db, nullptr);
  EXPECT_EQ(Children(), (std::vector<std::string>{"000001.wal", "LOCK"}));
}

TEST_F(DBTest, SecondOpenOfSameDirectoryFailsCleanly) {
  std::unique_ptr<DB> first;
  ASSERT_TRUE(DB::Open(Options(), path_, &first).ok());

  std::unique_ptr<DB> second;
  const Status s = DB::Open(Options(), path_, &second);
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_NE(s.message().find("LOCK"), std::string::npos) << s.ToString();
  EXPECT_EQ(second, nullptr);
  // The failed open left no new files behind.
  EXPECT_EQ(Children(), (std::vector<std::string>{"000001.wal", "LOCK"}));
}

TEST_F(DBTest, OpenFromAnotherProcessFailsWhileDirectoryIsHeld) {
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(Options(), path_, &db).ok());

  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    std::unique_ptr<DB> child_db;
    const Status s = DB::Open(Options(), path_, &child_db);
    ::_exit(s.IsIOError() && child_db == nullptr ? 0 : 1);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST_F(DBTest, ReopenAfterCloseStartsANewWalAndKeepsTheOldOne) {
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(Options(), path_, &db).ok());
  db.reset();
  ASSERT_TRUE(DB::Open(Options(), path_, &db).ok());
  EXPECT_EQ(Children(),
            (std::vector<std::string>{"000001.wal", "000002.wal", "LOCK"}));
}

TEST_F(DBTest, NewWalIsNumberedPastExistingFiles) {
  std::filesystem::create_directory(path_);
  std::ofstream(TableFileName(path_, 7)) << "table";
  std::ofstream(path_ + "/notes.txt") << "ignored";

  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(Options(), path_, &db).ok());
  EXPECT_EQ(Children(), (std::vector<std::string>{"000007.sst", "000008.wal",
                                                  "LOCK", "notes.txt"}));
  EXPECT_EQ(std::filesystem::file_size(TableFileName(path_, 7)), 5U);
}

TEST_F(DBTest, OpenFailsRatherThanWrapTheFileNumber) {
  std::filesystem::create_directory(path_);
  std::ofstream(WalFileName(path_, UINT64_MAX)) << "log";
  std::ofstream(WalFileName(path_, 0)) << "log";

  std::unique_ptr<DB> db;
  EXPECT_TRUE(DB::Open(Options(), path_, &db).IsCorruption());
  EXPECT_EQ(db, nullptr);
  EXPECT_EQ(std::filesystem::file_size(WalFileName(path_, 0)), 3U);
}

TEST_F(DBTest, OpenWithTrailingSlashWorks) {
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(Options(), path_ + "/", &db).ok());
  EXPECT_EQ(Children(), (std::vector<std::string>{"000001.wal", "LOCK"}));
}

TEST_F(DBTest, OpenWithMissingParentIsIOError) {
  std::unique_ptr<DB> db;
  EXPECT_TRUE(DB::Open(Options(), (root_ / "missing" / "db").string(), &db)
                  .IsIOError());
  EXPECT_EQ(db, nullptr);
}

TEST_F(DBTest, OpenOnRegularFileIsIOError) {
  std::ofstream(path_) << "not a directory";
  std::unique_ptr<DB> db;
  EXPECT_TRUE(DB::Open(Options(), path_, &db).IsIOError());
  EXPECT_EQ(db, nullptr);
}

}  // namespace
}  // namespace sediment
