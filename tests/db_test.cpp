#include "sediment/db.h"

#include <gtest/gtest.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
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

// The value Get returns for `key`, or "NOT_FOUND". Any other status fails the
// test.
std::string GetOrNotFound(DB* db, std::string_view key) {
  std::string value;
  const Status s = db->Get(key, &value);
  if (s.IsNotFound()) {
    return "NOT_FOUND";
  }
  EXPECT_TRUE(s.ok()) << s.ToString();
  return value;
}

TEST_F(DBTest, GetOnEmptyDatabaseIsNotFound) {
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(Options(), path_, &db).ok());
  EXPECT_EQ(GetOrNotFound(db.get(), "missing"), "NOT_FOUND");
}

TEST_F(DBTest, PutThenGetReturnsTheValue) {
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(Options(), path_, &db).ok());
  ASSERT_TRUE(db->Put("a", "1").ok());
  ASSERT_TRUE(db->Put("b", "2").ok());
  EXPECT_EQ(GetOrNotFound(db.get(), "a"), "1");
  EXPECT_EQ(GetOrNotFound(db.get(), "b"), "2");
  EXPECT_EQ(GetOrNotFound(db.get(), "c"), "NOT_FOUND");
}

TEST_F(DBTest, EmptyKeyAndValueAreStored) {
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(Options(), path_, &db).ok());
  ASSERT_TRUE(db->Put("", "").ok());
  EXPECT_EQ(GetOrNotFound(db.get(), ""), "");
}

TEST_F(DBTest, PutOverwritesAnEarlierValue) {
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(Options(), path_, &db).ok());
  ASSERT_TRUE(db->Put("k", "old").ok());
  ASSERT_TRUE(db->Put("k", "new").ok());
  EXPECT_EQ(GetOrNotFound(db.get(), "k"), "new");
}

TEST_F(DBTest, DeleteHidesTheValueAndPutRestoresIt) {
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(Options(), path_, &db).ok());
  ASSERT_TRUE(db->Put("k", "v1").ok());
  ASSERT_TRUE(db->Delete("k").ok());
  EXPECT_EQ(GetOrNotFound(db.get(), "k"), "NOT_FOUND");
  ASSERT_TRUE(db->Put("k", "v2").ok());
  EXPECT_EQ(GetOrNotFound(db.get(), "k"), "v2");
}

TEST_F(DBTest, DeleteOfMissingKeySucceeds) {
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(Options(), path_, &db).ok());
  EXPECT_TRUE(db->Delete("never-written").ok());
  EXPECT_EQ(GetOrNotFound(db.get(), "never-written"), "NOT_FOUND");
}

// Writes `count` keys, overwrites every third and deletes every fifth, so a
// reopen has to replay values, overwrites and tombstones in order.
void WriteMixedWorkload(DB* db, int count) {
  for (int i = 0; i < count; ++i) {
    ASSERT_TRUE(
        db->Put("key" + std::to_string(i), "v" + std::to_string(i)).ok());
  }
  for (int i = 0; i < count; i += 3) {
    ASSERT_TRUE(
        db->Put("key" + std::to_string(i), "w" + std::to_string(i)).ok());
  }
  for (int i = 0; i < count; i += 5) {
    ASSERT_TRUE(db->Delete("key" + std::to_string(i)).ok());
  }
}

void CheckMixedWorkload(DB* db, int count) {
  for (int i = 0; i < count; ++i) {
    const std::string key = "key" + std::to_string(i);
    std::string expected = "v" + std::to_string(i);
    if (i % 3 == 0) {
      expected = "w" + std::to_string(i);
    }
    if (i % 5 == 0) {
      expected = "NOT_FOUND";
    }
    EXPECT_EQ(GetOrNotFound(db, key), expected) << key;
  }
}

TEST_F(DBTest, WritesSurviveReopen) {
  constexpr int kCount = 1000;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(Options(), path_, &db).ok());
  WriteMixedWorkload(db.get(), kCount);
  db.reset();

  ASSERT_TRUE(DB::Open(Options(), path_, &db).ok());
  CheckMixedWorkload(db.get(), kCount);
}

TEST_F(DBTest, SyncedWritesSurviveReopen) {
  constexpr int kCount = 50;
  Options options;
  options.sync_writes = true;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(options, path_, &db).ok());
  WriteMixedWorkload(db.get(), kCount);
  db.reset();

  ASSERT_TRUE(DB::Open(options, path_, &db).ok());
  CheckMixedWorkload(db.get(), kCount);
}

TEST_F(DBTest, WritesAfterReopenAreNewerThanReplayedOnes) {
  // Each open replays every earlier log, so a write made after a reopen must
  // get a sequence number past the replayed ones to win on the next replay.
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(Options(), path_, &db).ok());
  ASSERT_TRUE(db->Put("k", "first").ok());
  ASSERT_TRUE(db->Put("gone", "x").ok());
  db.reset();

  ASSERT_TRUE(DB::Open(Options(), path_, &db).ok());
  ASSERT_TRUE(db->Put("k", "second").ok());
  ASSERT_TRUE(db->Delete("gone").ok());
  db.reset();

  ASSERT_TRUE(DB::Open(Options(), path_, &db).ok());
  EXPECT_EQ(GetOrNotFound(db.get(), "k"), "second");
  EXPECT_EQ(GetOrNotFound(db.get(), "gone"), "NOT_FOUND");
  ASSERT_TRUE(db->Put("gone", "back").ok());
  db.reset();

  ASSERT_TRUE(DB::Open(Options(), path_, &db).ok());
  EXPECT_EQ(GetOrNotFound(db.get(), "k"), "second");
  EXPECT_EQ(GetOrNotFound(db.get(), "gone"), "back");
  EXPECT_EQ(Children(),
            (std::vector<std::string>{"000001.wal", "000002.wal", "000003.wal",
                                      "000004.wal", "LOCK"}));
}

TEST_F(DBTest, WritesSurviveAProcessThatExitsWithoutClosing) {
  // The child skips the DB destructor, as a crash would. Unsynced writes are
  // already in the page cache, so they must still be replayed.
  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    std::unique_ptr<DB> child_db;
    bool ok = DB::Open(Options(), path_, &child_db).ok();
    for (int i = 0; ok && i < 100; ++i) {
      ok = child_db->Put("key" + std::to_string(i), "v" + std::to_string(i))
               .ok();
    }
    ::_exit(ok ? 0 : 1);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);

  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(Options(), path_, &db).ok());
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(GetOrNotFound(db.get(), "key" + std::to_string(i)),
              "v" + std::to_string(i));
  }
}

TEST_F(DBTest, TornTailOfTheLogIsDroppedOnReopen) {
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(Options(), path_, &db).ok());
  ASSERT_TRUE(db->Put("kept", "1").ok());
  ASSERT_TRUE(db->Put("torn", "2").ok());
  db.reset();

  // Cut the last record in half, as a crash partway through an append would.
  const std::string log = WalFileName(path_, 1);
  std::filesystem::resize_file(log, std::filesystem::file_size(log) - 4);

  ASSERT_TRUE(DB::Open(Options(), path_, &db).ok());
  EXPECT_EQ(GetOrNotFound(db.get(), "kept"), "1");
  EXPECT_EQ(GetOrNotFound(db.get(), "torn"), "NOT_FOUND");
  // The dropped record's sequence number is reused, which is safe because it
  // was never applied.
  ASSERT_TRUE(db->Put("after", "3").ok());
  db.reset();

  ASSERT_TRUE(DB::Open(Options(), path_, &db).ok());
  EXPECT_EQ(GetOrNotFound(db.get(), "kept"), "1");
  EXPECT_EQ(GetOrNotFound(db.get(), "torn"), "NOT_FOUND");
  EXPECT_EQ(GetOrNotFound(db.get(), "after"), "3");
}

TEST_F(DBTest, CorruptLogFailsOpen) {
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(Options(), path_, &db).ok());
  ASSERT_TRUE(db->Put("a", "1").ok());
  ASSERT_TRUE(db->Put("b", "2").ok());
  db.reset();

  // Flip a byte in the first record's payload. The intact record after it
  // rules out a torn tail, so dropping it silently would lose a write.
  const std::string log = WalFileName(path_, 1);
  {
    std::fstream f(log, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(12);
    f.put('\xff');
  }

  const Status s = DB::Open(Options(), path_, &db);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_EQ(db, nullptr);
  // The failed open created no new log.
  EXPECT_EQ(Children(), (std::vector<std::string>{"000001.wal", "LOCK"}));
}

// A read-only mapping of `size` zero bytes that are never touched, so a test
// can pass a key or value over 4 GiB without allocating it.
class HugeBuffer {
 public:
  explicit HugeBuffer(size_t size)
      : size_(size),
        data_(::mmap(nullptr, size, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0)) {
  }
  ~HugeBuffer() {
    if (data_ != MAP_FAILED) {
      ::munmap(data_, size_);
    }
  }
  HugeBuffer(const HugeBuffer&) = delete;
  HugeBuffer& operator=(const HugeBuffer&) = delete;

  bool ok() const { return data_ != MAP_FAILED; }
  std::string_view view() const {
    return {static_cast<const char*>(data_), size_};
  }

 private:
  size_t size_;
  void* data_;
};

TEST_F(DBTest, OversizedKeyOrValueIsInvalidArgument) {
  // Keys need room for the 8-byte tag within a 32-bit length.
  HugeBuffer key(size_t{UINT32_MAX} - 7);
  HugeBuffer value(size_t{UINT32_MAX} + 1);
  if (!key.ok() || !value.ok()) {
    GTEST_SKIP() << "cannot reserve 8 GiB of address space";
  }

  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(Options(), path_, &db).ok());
  EXPECT_TRUE(db->Put(key.view(), "v").IsInvalidArgument());
  EXPECT_TRUE(db->Put("k", value.view()).IsInvalidArgument());
  EXPECT_TRUE(db->Delete(key.view()).IsInvalidArgument());
  std::string unused;
  EXPECT_TRUE(db->Get(key.view(), &unused).IsNotFound());

  // Nothing reached the log, and the DB still accepts writes.
  ASSERT_TRUE(db->Put("k", "v").ok());
  db.reset();
  ASSERT_TRUE(DB::Open(Options(), path_, &db).ok());
  EXPECT_EQ(GetOrNotFound(db.get(), "k"), "v");
}

}  // namespace
}  // namespace sediment
