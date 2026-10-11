# sediment-kv

[![CI](https://github.com/c0drman0o0o/sediment-kv/actions/workflows/ci.yml/badge.svg)](https://github.com/c0drman0o0o/sediment-kv/actions/workflows/ci.yml)

A persistent, crash-safe key-value store written in C++20, built on a log-structured merge-tree (LSM-tree): write-ahead log, memtable, SSTables and leveled compaction.

> **Status: work in progress.** Only the foundations exist so far (build system, CI, the `Status` type). There is no database API yet, so there is nothing to use beyond building and running the tests. Usage examples will be added once `DB::Open`, `Put` and `Get` land.

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

### Sanitizers

Set `SEDIMENT_SANITIZE` to build with sanitizers: `address`, `undefined`, `thread`, or a comma-separated combination. `thread` cannot be combined with `address`.

```bash
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DSEDIMENT_SANITIZE=address,undefined
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

CI runs the same steps on clang and gcc, plus an ASan + UBSan job.

## Repository layout

| Path | Contents |
|------|----------|
| `include/sediment/` | Public headers |
| `src/` | Library implementation |
| `tests/` | GoogleTest unit tests |
| `bench/`, `fuzz/`, `tools/` | Reserved for benchmarks, fuzz targets and command-line tools |

## Design

Design decisions and the alternatives that were rejected are recorded in [DESIGN.md](DESIGN.md), along with background reading on key-value storage engines.
