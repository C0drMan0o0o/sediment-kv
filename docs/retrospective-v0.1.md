# v0.1 retrospective

v0.1 is the in-memory half of the engine: `Status`, encoding helpers, arena, skiplist, internal keys and the memtable. Nothing touches disk yet.

## What went well

- **Layering paid off.** The memtable was thin because each piece under it was already tested: the skiplist is generic over a comparator, and internal-key ordering (user key ascending, sequence descending) was settled and tested earlier. `Get` is one seek plus a check.
- **Tombstones are an entry, not a special case.** Treating a delete as a `kDeletion` version meant the lookup path needed no extra machinery. The only new idea was a distinct "deleted" result, so callers can tell "stop, the key is gone" from "ask an older source".
- **Strict tooling caught real issues early.** `-Werror`, ASan/UBSan and clang-tidy in CI ran from the start. clang-tidy flagged uninitialised pointers and an implicit widening multiplication in the memtable PR, both cheap to fix now.
- **Decisions are written down.** `DESIGN.md` records the choice and the rejected alternative for byte views, error handling, key encoding and the memtable.

## What went badly

- **One test was wrong, not the code.** The memory-usage test assumed one small entry would raise `ApproximateMemoryUsage()`. The arena grows in 4 KB blocks, so it did not. The fix was to assert on the total across many entries. Lesson: write the test against the documented granularity, not the intuition.
- **CI tooling ran late.** clang-tidy only ran in CI, so its findings arrived after the PR was open. It should be run locally before pushing.
- **The memtable is only partly proven.** It has unit tests with hand-picked cases but no randomised oracle test against `std::map` like the skiplist has. A model-based test over random puts and deletes would be a stronger check.
- **Memory accounting is coarse.** The usage figure includes arena slack and moves in 4 KB steps. That is fine for a megabyte-scale flush threshold but would be misleading for small limits.

## Carry forward

- Run `clang-tidy` locally before every push.
- Add a randomised memtable-versus-`std::map` test (with sequences and tombstones) before relying on it in the write path.
- Next: CRC32C, the file layer and the write-ahead log.
