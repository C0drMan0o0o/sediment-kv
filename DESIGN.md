# Sediment: Design Notes

Decisions are recorded here as they are made, with the alternatives that were rejected.

## Trade-offs

| Area | Chosen | Rejected | Why |
|------|--------|----------|-----|
| Byte views | `std::string_view` | A custom `Slice` class | See [Byte views](#byte-views) |
| Error reporting | `Status` return values, `[[nodiscard]]` | Exceptions | See [Error handling](#error-handling) |
| Memtable structure | Skiplist over an arena | Red-black tree, `std::map` | See [Memtable](#memtable) |
| Key versions | Internal key = user key + (sequence, type) tag | Overwrite in place | See [Key encoding](#key-encoding) |

## Byte views

Keys and values are arbitrary bytes, passed around as `std::string_view`.

LevelDB and RocksDB use their own `Slice` because they predate C++17. The standard type gives the same non-owning (pointer, length) semantics, is binary-safe (the length is stored, so embedded `\0` bytes are fine), and works with `std::string`, comparison, hashing and the rest of the standard library without any code to write or maintain. A custom `Slice` would add a class and no capability we need.

Rules that follow from this:

- Never build a `string_view` from a `const char*` without an explicit length. That stops at the first `\0` and silently truncates binary data.
- A view does not own its bytes. Anything that holds a view past the call that produced it must be able to say who owns the underlying storage (memtable arena, cached block, caller's buffer). Types that outlive their inputs, such as `Status`, copy the bytes into a `std::string`.

## Error handling

Every fallible operation returns a `Status`: a code (`OK`, `NotFound`, `Corruption`, `IOError`, `InvalidArgument`) plus an optional message. No exceptions cross the public API.

- `Status` is `[[nodiscard]]`, so ignoring a returned error is a compile error under `-Werror`. For a storage engine, a dropped error is silent data loss.
- The message is stored as a `std::string`, so a `Status` can safely outlive the buffers it was built from.
- The success path is cheap: an OK `Status` holds an empty message.
What each code means:

| Code | Meaning | Example | Expected? |
|------|---------|---------|-----------|
| `OK` | The operation succeeded. | `Put` accepted and logged. | Yes |
| `NotFound` | The key (or file) does not exist, or a tombstone hides it. | `Get` on a deleted key. | Yes, a normal lookup result |
| `Corruption` | Stored bytes failed an integrity check. | Bad block CRC, bad magic number, truncated record. | No, but possible after a crash or disk fault |
| `IOError` | The operating system refused an operation; the data may be fine. | `open`, `write` or `fsync` failed; disk full. | No |
| `InvalidArgument` | The caller passed something the API forbids. | An option with a nonsensical value, such as a zero-byte block size. | No, it is a caller bug |

Rules for choosing a code:

- `NotFound` is not an error of the engine. Callers branch on it, so never return it for anything but a missing key or file.
- `Corruption` and `IOError` are different failures. If the read succeeded and the bytes are wrong, it is `Corruption`; if the read itself failed, it is `IOError`. A reader must never hand back garbage in place of either.
- A torn tail in the write-ahead log is not `Corruption`. Recovery stops at the first bad record and treats the rest as never acknowledged.

## Key encoding

The engine never overwrites or removes an entry in memory or in an SSTable. Every write is a new version identified by an internal key:

```
internal key = user_key bytes | fixed64 LE ( sequence << 8 | type )
```

- `sequence` is a 56-bit number assigned to each write, increasing. `type` is `kValue` (1) or `kDeletion` (0). The numeric values are part of the on-disk format.
- Ordering: user key ascending (bytewise), then sequence **descending**, so the newest version of a key comes first. A lookup seeks to `(key, read_sequence)` and the first entry at or after it is the newest version visible to that reader.
- A delete is a `kDeletion` entry (a tombstone), not a removal. It must outlive the values it hides, because older versions may still sit in lower levels. Dropping tombstones is a compaction decision.
- Sequence numbers are what make snapshots possible later: a reader pins a sequence and ignores anything newer.

Rejected: overwriting in place. It cannot represent deletes across files, and it makes the memtable's contents depend on write order in ways that break snapshots and recovery.

## Memtable

The memtable is the in-memory write buffer. Writes go to the WAL first, then into the memtable; when it reaches `memtable_bytes` it becomes immutable and is flushed to an SSTable.

**Structure: skiplist over an arena.**

- A skiplist keeps entries sorted, which is exactly what the flush needs (SSTables are written in key order) and gives ordered iteration for free.
- Nodes and entry bytes live in an `Arena`: allocation is a pointer bump, memory is freed all at once when the memtable is dropped, and `MemoryUsage()` gives the flush trigger without walking the structure.
- Red-black trees and `std::map` were rejected: per-node `new`/`delete`, worse locality, and no path to lock-free concurrent reads. A skiplist with a single writer needs only release/acquire on the next-pointers to allow readers during writes; that is a later extension, and v0.1 is single-threaded.

**Entry layout** (one arena allocation per write; the skiplist stores a pointer to it):

```
varint32 internal_key_len | internal key | varint32 value_len | value
```

**API.**

- `Add(sequence, type, key, value)` inserts a new version. Adding the same `(key, sequence)` twice is a caller bug.
- `Get(key, sequence, &value)` returns one of three results: `kFound` (value copied out), `kDeleted` (a tombstone is the newest visible version), `kNotFound` (no visible version in this memtable).
- `kDeleted` and `kNotFound` must stay distinct. `kNotFound` means "ask the next, older source" (immutable memtable, then SSTables); `kDeleted` means "stop, the key is gone". Collapsing them would resurrect deleted keys.
- `ApproximateMemoryUsage()` is the arena's total, including slack in partially filled blocks. It grows in 4 KB steps, which is fine for a flush threshold of megabytes.

## Files and durability

`WritableFile`, `SequentialFile` and `RandomAccessFile` (`sediment/file.h`) are thin wrappers over POSIX file descriptors. Every call returns a `Status`; `IOError` messages carry the path and `strerror`.

**Appends are unbuffered.** `WritableFile::Append` goes straight to `write(2)`, retrying on `EINTR` and short writes. Data is then in the operating system's page cache, which survives a process crash (`kill -9`) but not power loss. Only `Sync()` makes it durable.

**The macOS `fsync` caveat.** On macOS, `fsync(2)` only hands data to the drive; the drive may keep it in a volatile write cache and lose it on power failure, so `fsync` does not meet the durability promise `sync_writes` makes. `Sync()` therefore calls `fcntl(fd, F_FULLFSYNC)`, which also flushes the drive's cache. It is slower, and a few file systems reject it, so `Sync()` falls back to `fsync` when `F_FULLFSYNC` fails. On Linux, `fsync(2)` already waits for the device to report the data durable, so it is used directly.

**Not yet handled.** `Sync()` on a newly created file does not sync the parent directory, so the file's directory entry could be lost in a crash. This matters for the manifest and `CURRENT` file and is deferred until they exist.

## Write-ahead log

`wal::Writer` (`sediment/wal.h`) appends records to a log file. Each record is one frame:

```
[masked crc32c: 4][length: 4][type: 1][payload: length]
```

Fixed-width fields are little-endian. The CRC (masked, as in LevelDB) covers the type byte and payload. `type` is always `kFull` (1) today; 0 is reserved so a zero-filled tail from a torn write never parses as a valid record.

**Length-prefixed, not block-fragmented.** LevelDB splits records across 32 KB blocks so a reader can resynchronise at the next block boundary after corruption. Sediment skips that: a record is a single frame of any size, which keeps the writer and reader simple. The cost is that after a bad frame the reader cannot find the next one, so recovery stops at the first damaged record and drops everything after it. That is acceptable because the log is only replayed to rebuild the memtable after a crash, and a torn write can only damage the tail. The `type` byte leaves room to add fragment types later without changing the header.

**Durability.** `AddRecord(payload, sync)` writes the header and payload with two unbuffered appends, then calls `Sync()` only if `sync` is true. After any failed append the log may end in a partial frame, so the writer returns the same error for all later appends rather than writing valid frames behind garbage.

**Reading and torn writes.** `wal::Reader` returns records in order and stops at the first frame that is damaged: a short header, a short payload, a reserved or unknown type byte (a zero-filled tail lands here), or a CRC mismatch. It never returns a damaged record. After stopping it reads the rest of the file so `dropped_bytes()` reports exactly how much was discarded, and `stop_reason()` says why. Damage is not an error (`status()` stays OK), because a torn tail is the expected result of a crash; only a failing `read(2)` sets a non-OK status. Payloads are read in 64 KB chunks, so a corrupted length field on a small file ends in a short read instead of a multi-gigabyte allocation.

## Recovery

A WAL record holds one write:

```
[sequence: fixed64][type: 1][key_len: varint32][key][value_len: varint32][value]
```

`RecoverFromLog` (`sediment/recovery.h`) replays the log into a memtable in order and returns `RecoveryStats`: entries applied, the highest sequence seen (where the next write continues from), and the bytes dropped at a damaged tail with the reason. A torn tail is expected after a crash and is not an error. An intact frame (CRC verified) whose entry will not decode is `Corruption`, because a torn write cannot produce a valid CRC over bad contents.

**Kill test.** To run it (100 iterations here):

```bash
cmake -S . -B build && cmake --build build
tools/kill_recover.sh build/crash_demo 100
tools/kill_recover.sh build/crash_demo 100 nosync
```

`tools/kill_recover.sh` runs `crash_demo write`, which appends synced puts and prints `ack <n>` once each is durable. The script sends `kill -9` after a random 20-220 ms, then `crash_demo verify` replays the log and checks that every acknowledged write was recovered with the right value. Recovering one more write than was acknowledged is fine: the process can die after the sync but before the print. `kill -9` only proves the log survives a process crash, since the page cache outlives the process; surviving power loss needs the fault-injecting file layer.

The default `sync` mode rarely produces a torn tail: the writer spends its time blocked in `fsync`, so a kill seldom lands between a frame's header and payload writes (3 in 1000 iterations in one run). `nosync` mode keeps the writer out of `fsync`, so kills often land mid-frame (38 in 100 in one run). The run then also fails unless at least one iteration recovered past a torn tail, which shows the torn-tail path was exercised.

## SSTable blocks

A data block holds sorted entries with shared-prefix compression, in the LevelDB layout. Each entry is `[shared][non_shared][value_length]` as varint32s, then the key suffix and the value. Every 16th entry (the restart interval) stores its key in full and its offset goes into the restart array, so a reader can binary search the restarts and then scan at most 16 entries.

The block ends with the restart offsets (fixed32 each), the restart count (fixed32) and a masked CRC32C over everything before it. `BlockBuilder` compares nothing: the caller adds keys in comparator order, and prefix sharing is bytewise.

**Reading.** `BlockReader` is a non-owning view over a finished block; the bytes must outlive it and its iterators. Opening it verifies the CRC and that the restart count fits in the block, so a flipped byte or truncation gives `Corruption` and iterators are never valid. `Seek` binary-searches the restart points (each holds a full key, so it can be compared directly) for the last one below the target, then scans forward at most one restart interval. `SeekToFirst` and `Next` decode entries in order, rebuilding each key from the previous one's shared prefix. An entry whose lengths run past the block ends iteration with `Corruption` instead of reading out of bounds. The comparator is a function pointer defaulting to bytewise, and it must order keys the way the block's writer did.

## SSTable files

A table file is the data blocks back to back, then one index block, then a fixed 28-byte footer:

```
[data block 0] ... [data block N-1] [index block] [footer]
```

Every block is a `BlockBuilder` block, so each carries its own CRC and there is no separate per-block trailer. `TableBuilder` appends entries to the current data block and finishes it once it reaches `block_size` (4 KB by default). The block is then written and its position recorded as a handle (`offset`, `size`, where `size` includes the block's CRC).

The index block has one entry per data block: the key is the block's last key, the value is the handle as two varint64s. A reader can binary search the index for the first block whose last key is >= the target, and read only that block. LevelDB shortens these keys to the shortest separator between neighbouring blocks to save space; Sediment stores the exact last key, which is simpler and costs little at 4 KB blocks.

The footer holds the index handle (two fixed64s), a format version (fixed32) and a magic number (fixed64, which spells `sediment` in the file). A reader seeks to `file_size - 28`, so it needs nothing else to find the index. A wrong length, magic number or version is `Corruption`.

`TableBuilder` records the smallest and largest key and the entry count, which the version set will need for file metadata. Keys must arrive in strictly increasing order. The builder does not sync or close the file; `WriteMemTableToTable` does that after `Finish()`, so a memtable flush can treat a successful return as durable. Table keys are encoded internal keys, so tombstones and older versions are written as they are; hiding them is the job of the read path and compaction.

## Resources

Background reading on key-value storage engines.

### Tutorials

- [mini-lsm](https://skyzh.github.io/mini-lsm/): a course that builds an LSM-tree KV store from scratch (memtable, SSTables, compaction, WAL, MVCC).
- [Let's Build a Simple Database](https://cstack.github.io/db_tutorial/): an on-disk B-tree, step by step.
- [Bitcask paper](https://riak.com/assets/bitcask-intro.pdf): an append-only log with an in-memory hash index.

### Papers

- [Bigtable](https://research.google/pubs/bigtable-a-distributed-storage-system-for-structured-data/): the memtable/SSTable design.
- [The Log-Structured Merge-Tree](https://www.cs.umb.edu/~poneil/lsmtree.pdf) (O'Neil et al., 1996).
- [WiscKey](https://www.usenix.org/system/files/conference/fast16/fast16-papers-lu.pdf): separating keys from values to reduce write amplification.

### Codebases and docs

- [LevelDB](https://github.com/google/leveldb): small, readable C++; see `doc/impl.md` and `doc/table_format.md`.
- [RocksDB wiki](https://github.com/facebook/rocksdb/wiki): compaction styles, the write path and tuning.
