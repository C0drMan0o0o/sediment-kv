#include "sediment/status.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>

namespace sediment {
namespace {

TEST(Status, DefaultConstructedIsOk) {
  Status s;
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(s.code(), Status::Code::kOk);
  EXPECT_TRUE(s.message().empty());
  EXPECT_EQ(s.ToString(), "OK");
}

TEST(Status, OkFactoryMatchesDefault) {
  EXPECT_TRUE(Status::OK().ok());
}

TEST(Status, EachErrorKindSetsOnlyItsPredicate) {
  const Status not_found = Status::NotFound("nf");
  const Status corruption = Status::Corruption("co");
  const Status io_error = Status::IOError("io");
  const Status invalid = Status::InvalidArgument("ia");

  EXPECT_TRUE(not_found.IsNotFound());
  EXPECT_FALSE(not_found.IsCorruption() || not_found.IsIOError() ||
               not_found.IsInvalidArgument() || not_found.ok());

  EXPECT_TRUE(corruption.IsCorruption());
  EXPECT_FALSE(corruption.IsNotFound() || corruption.IsIOError() ||
               corruption.IsInvalidArgument() || corruption.ok());

  EXPECT_TRUE(io_error.IsIOError());
  EXPECT_FALSE(io_error.IsNotFound() || io_error.IsCorruption() ||
               io_error.IsInvalidArgument() || io_error.ok());

  EXPECT_TRUE(invalid.IsInvalidArgument());
  EXPECT_FALSE(invalid.IsNotFound() || invalid.IsCorruption() ||
               invalid.IsIOError() || invalid.ok());
}

TEST(Status, ErrorsAreNotOk) {
  EXPECT_FALSE(Status::NotFound().ok());
  EXPECT_FALSE(Status::Corruption().ok());
  EXPECT_FALSE(Status::IOError().ok());
  EXPECT_FALSE(Status::InvalidArgument().ok());
}

TEST(Status, KeepsMessage) {
  const Status s = Status::IOError("disk on fire");
  EXPECT_EQ(s.message(), "disk on fire");
}

TEST(Status, ToStringIncludesKindAndMessage) {
  EXPECT_EQ(Status::NotFound("key").ToString(), "NotFound: key");
  EXPECT_EQ(Status::Corruption("bad crc").ToString(), "Corruption: bad crc");
  EXPECT_EQ(Status::IOError("open failed").ToString(), "IOError: open failed");
  EXPECT_EQ(Status::InvalidArgument("empty key").ToString(),
            "InvalidArgument: empty key");
}

TEST(Status, ToStringOmitsSeparatorWhenMessageEmpty) {
  EXPECT_EQ(Status::NotFound().ToString(), "NotFound");
}

TEST(Status, MessageMayContainEmbeddedNul) {
  const std::string raw("a\0b", 3);
  const Status s = Status::Corruption(raw);
  EXPECT_EQ(s.message().size(), 3U);
  EXPECT_EQ(s.message(), std::string_view(raw));
}

TEST(Status, CopyAndMovePreserveValue) {
  const Status original = Status::Corruption("bad block");

  Status copy = original;
  EXPECT_TRUE(copy.IsCorruption());
  EXPECT_EQ(copy.message(), "bad block");

  Status moved = std::move(copy);
  EXPECT_TRUE(moved.IsCorruption());
  EXPECT_EQ(moved.message(), "bad block");
}

TEST(Status, EqualityComparesCodeAndMessage) {
  EXPECT_EQ(Status::OK(), Status());
  EXPECT_EQ(Status::NotFound("k"), Status::NotFound("k"));
  EXPECT_NE(Status::NotFound("k"), Status::NotFound("other"));
  EXPECT_NE(Status::NotFound("k"), Status::Corruption("k"));
}

}  // namespace
}  // namespace sediment
