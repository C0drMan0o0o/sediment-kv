# Sediment: Design Notes

Decisions are recorded here as they are made, with the alternatives that were rejected.

## Trade-offs

| Area | Chosen | Rejected | Why |
|------|--------|----------|-----|
| Byte views | `std::string_view` | A custom `Slice` class | See [Byte views](#byte-views) |
| Error reporting | `Status` return values, `[[nodiscard]]` | Exceptions | See [Error handling](#error-handling) |

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
