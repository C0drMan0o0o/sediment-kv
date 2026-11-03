# sediment-kv

[![CI](https://github.com/c0drman0o0o/sediment-kv/actions/workflows/ci.yml/badge.svg)](https://github.com/c0drman0o0o/sediment-kv/actions/workflows/ci.yml)

A persistent, crash-safe key-value store written in C++20, built on a log-structured merge-tree (LSM-tree): write-ahead log, memtable, SSTables and leveled compaction.

> **Status: work in progress.** The storage building blocks exist and are tested on their own: a memtable, a write-ahead log with crash recovery, and SSTables that a memtable can be flushed to and looked up in. They are not wired together yet and there is no database API, so there is nothing to use beyond building and running the tests. Next up: a table iterator, then `DB::Open`, `Put`, `Get` and `Delete`. Usage examples will be added once those land.

## Components

| Component | Header | Purpose |
|-----------|--------|---------|
| `Status` | `status.h` | Error handling without exceptions |
| Encoding helpers | `coding.h` | Fixed-width and varint encoding |
| `Arena` | `arena.h` | Block-based allocator backing the memtable |
| `SkipList` | `skiplist.h` | Ordered in-memory structure, generic over a comparator |
| Internal keys | `internal_key.h` | User key, sequence number and type, ordered by user key ascending then sequence descending |
| `MemTable` | `memtable.h` | Write buffer; deletes are stored as tombstone entries |
| CRC32C | `crc32c.h` | Checksums for log records and SSTable blocks |
| File layer | `file.h` | POSIX file wrappers that return `Status`; `Sync` (with `F_FULLFSYNC` on macOS) and `SyncDir` |
| Write-ahead log | `wal.h` | CRC-checked log records; the reader drops a torn tail and reports damage elsewhere as corruption |
| Recovery | `recovery.h` | Replays a write-ahead log into a memtable |
| Blocks | `block_builder.h`, `block_reader.h` | Sorted key/value blocks with prefix compression, restart points and a CRC |
| SSTables | `table_builder.h`, `table_reader.h` | Table files of data blocks, an index block and a footer; a memtable flushes to one, and lookups find the newest visible version of a user key |

## Requirements

- CMake 4.2 or newer
- A C++20 compiler (clang or gcc)
- Ninja (optional; any CMake generator works)
- Network access on the first configure, to fetch GoogleTest

## Build and test

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Warnings are errors (`-Wall -Wextra -Wpedantic -Werror`) for the project's own targets.

### Sanitisers

Set `SEDIMENT_SANITIZE` to build with sanitisers: `address`, `undefined`, `thread`, or a comma-separated combination. `thread` cannot be combined with `address`.

```bash
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DSEDIMENT_SANITIZE=address,undefined
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

CI runs the same steps on clang and gcc, plus an ASan + UBSan job and a clang-format + clang-tidy job pinned to LLVM 23.1.3.

## Repository layout

| Path | Contents |
|------|----------|
| `include/sediment/` | Public headers |
| `src/` | Library implementation |
| `tests/` | GoogleTest unit tests |
| `tools/` | `crash_demo` and `kill_recover.sh`, a `kill -9` crash-recovery test for the write-ahead log (see [Recovery](DESIGN.md#recovery)) |
| `docs/` | Release retrospectives |
| `bench/`, `fuzz/` | Reserved for benchmarks and fuzz targets |

## Design

Design decisions and the alternatives that were rejected are recorded in [DESIGN.md](DESIGN.md), along with background reading on key-value storage engines. Lessons from the first release are in the [v0.1 retrospective](docs/retrospective-v0.1.md).
