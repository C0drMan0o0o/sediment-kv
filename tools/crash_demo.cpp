// Crash-recovery demo driver.
//
//   crash_demo write <wal> [nosync]
//                              Append puts forever, printing "ack <n>" after
//                              each one is appended. Puts are synced unless
//                              "nosync" is given, which keeps the writer out
//                              of fsync so a kill can land between the header
//                              and payload writes and leave a torn tail.
//                              Meant to be killed.
//   crash_demo verify <wal> <acked>
//                              Replay the log and check that every one of the
//                              first <acked> writes survived with its value.
//
// Write i (1-based) stores key "key<i>" with value "value<i>" at sequence i.

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>

#include "sediment/file.h"
#include "sediment/memtable.h"
#include "sediment/recovery.h"
#include "sediment/wal.h"

namespace {

std::string KeyFor(uint64_t i) {
  return "key" + std::to_string(i);
}
std::string ValueFor(uint64_t i) {
  return "value" + std::to_string(i);
}

int Write(const std::string& path, bool sync) {
  std::unique_ptr<sediment::WritableFile> file;
  sediment::Status s = sediment::NewWritableFile(path, &file);
  if (!s.ok()) {
    std::fprintf(stderr, "%s\n", s.ToString().c_str());
    return 1;
  }
  sediment::wal::Writer writer(std::move(file));
  for (uint64_t i = 1;; ++i) {
    std::string entry;
    sediment::EncodeLogEntry(&entry, i, sediment::ValueType::kValue, KeyFor(i),
                             ValueFor(i));
    s = writer.AddRecord(entry, sync);
    if (!s.ok()) {
      std::fprintf(stderr, "%s\n", s.ToString().c_str());
      return 1;
    }
    std::printf("ack %llu\n", static_cast<unsigned long long>(i));
    std::fflush(stdout);
  }
}

int Verify(const std::string& path, uint64_t acked) {
  sediment::MemTable mem;
  sediment::RecoveryStats stats;
  sediment::Status s = sediment::RecoverFromLog(path, &mem, &stats);
  if (!s.ok()) {
    std::fprintf(stderr, "recovery failed: %s\n", s.ToString().c_str());
    return 1;
  }
  if (stats.records < acked) {
    std::fprintf(stderr,
                 "lost acknowledged writes: acked %llu, recovered %llu\n",
                 static_cast<unsigned long long>(acked),
                 static_cast<unsigned long long>(stats.records));
    return 1;
  }
  for (uint64_t i = 1; i <= stats.records; ++i) {
    std::string value;
    if (mem.Get(KeyFor(i), stats.last_sequence, &value) !=
            sediment::MemTable::GetResult::kFound ||
        value != ValueFor(i)) {
      std::fprintf(stderr, "wrong or missing value for %s\n",
                   KeyFor(i).c_str());
      return 1;
    }
  }
  std::printf("recovered %llu (acked %llu, dropped %llu bytes)\n",
              static_cast<unsigned long long>(stats.records),
              static_cast<unsigned long long>(acked),
              static_cast<unsigned long long>(stats.dropped_bytes));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  // Building keys and values can throw std::bad_alloc; a demo tool cannot
  // recover from that, so report it and exit non-zero.
  try {
    if (argc == 3 && std::string(argv[1]) == "write") {
      return Write(argv[2], /*sync=*/true);
    }
    if (argc == 4 && std::string(argv[1]) == "write" &&
        std::string(argv[3]) == "nosync") {
      return Write(argv[2], /*sync=*/false);
    }
    if (argc == 4 && std::string(argv[1]) == "verify") {
      return Verify(argv[2], std::strtoull(argv[3], nullptr, 10));
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fatal: %s\n", e.what());
    return 1;
  }
  std::fprintf(
      stderr,
      "usage: crash_demo write <wal> [nosync] | crash_demo verify <wal> "
      "<acked>\n");
  return 2;
}
