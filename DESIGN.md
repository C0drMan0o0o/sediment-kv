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
