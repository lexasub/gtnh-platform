# Audit gp-8lc — LMDB chunk store read/write paths for error swallowing

**Date:** 2026-09-26 · **Scope:** `src/apps/chunk_store/**` + `src/apps/entity_state_store/**` · **Mode:** read-only
**Brief-specific questions answered:** what happens to a write that fails because the map is full;
is the AGENTS.md zero-copy claim `FlatBuffer → LMDB mmap → TCP send buffer` actually error-free.

## Method

```bash
cd /home/su/src/local/gtnh-platform
grep -rn "mdb_" --include=*.cpp --include=*.h --include=*.hpp src/ | wc -l     # 127
grep -rln "mdb_" --include=*.cpp --include=*.h --include=*.hpp src/           # 12 files
# every mdb_* call in the store, then its caller, then its caller's caller
awk 'NR>=1 && NR<=197'  src/apps/chunk_store/Storage/disk/LmdbStore.cpp
awk 'NR>=1 && NR<=212'  src/apps/entity_state_store/EntityStateStorage.cpp
```

**Live verification.** I compiled a standalone probe that ports `writeRaw` and `writeBatch`
line-for-line and drove them to `MDB_MAP_FULL` against a real LMDB
(`/home/su/.hermes/cache/scratch/lmdb_probe/probe.cpp`, built against
`~/.conan2/p/b/lmdb6c993bdf57bdb/.../liblmdb.a`). Results are quoted inline below as **[probe]**.

## The 127 `mdb_*` calls, classified

| File | `mdb_*` calls | Checked? |
|---|---|---|
| `Storage/disk/LmdbStore.cpp` | 28 | 22 yes, **6 not** |
| `src/apps/entity_state_store/EntityStateStorage.cpp` | 24 | 24 yes (but logged at `debug`) |
| `Storage/CASHandler.cpp` / `ChunkStore.cpp` / `EncodePipeline.cpp` / `FlushPipeline.*` / headers | rest | no direct `mdb_*` |

`EntityStateStorage.cpp` is the best-handled LMDB code in the repo — **every** `mdb_txn_begin`,
`mdb_dbi_open`, `mdb_get`, `mdb_put`, `mdb_del`, `mdb_txn_commit` and `mdb_env_*` return is tested
(lines 19-34, 99-129, 140-170, 182-209). Its one systematic flaw is severity: every failure is reported
at `spdlog::debug` (lines 101, 142, 168, 184, 207), which is invisible in a default `info`-level run.
A disk failure on the entity-state save path is therefore silent in production logs.

`LmdbStore.cpp` is where the swallowing lives.

---

## Finding 1 — a failed `mdb_txn_commit` is reported to the caller as success  · SEVERITY: high

`src/apps/chunk_store/Storage/disk/LmdbStore.cpp:13-16` (macro), `:113`, `:187`

```cpp
#define LMDB_TX_COMMIT() \
    if (int rc = mdb_txn_commit(txn); rc != 0) [[unlikely]] { \
        spdlog::error("mdb_txn_commit failed: {}", mdb_strerror(rc)); \
    }
```

The macro has no `return` and no `else`. Control falls through:

- `writeRaw` line 113 → `return true;` (line 114)
- `writeBatch` line 187 → `items.clear(); return true;` (lines 188-189)

Every caller that trusts the bool is wrong when a commit fails. This is the "error observed, logged,
and then structurally discarded" shape, one macro away from the wire.

**Consequence:** the flush thread logs "flushed N dirty chunks to LMDB"
(`FlushPipeline.cpp:70-71`, driven by `saved++` on the strength of the returned bool) and the client
gets a successful `SaveChunkResp` for a write that never reached disk.

## Finding 2 — MAP_FULL on the batch path: no growth, no retry, and the caller discards the `false`  · SEVERITY: high

**This is the direct answer to "what happens to a write that fails because the map is full."**

`writeRaw` (single-chunk) handles it. `LmdbStore.cpp:98-105`:

```cpp
        int rc = mdb_put(txn, dbi_, &key_val, &data_val, 0);
        if (rc == MDB_MAP_FULL) {
            mdb_txn_abort(txn);
            txn = nullptr;
            if (!growMapSize())
                return false;
            continue;                       // retry once after growing
        }
```

`writeBatch` does **not** (`:181-185`):

```cpp
        if (int rc = mdb_put(txn, dbi_, &key_val, &data_val, 0); rc != 0) {
            spdlog::error("writeBatch: mdb_put failed: {}", mdb_strerror(rc));
            mdb_txn_abort(txn);
            return false;                   // no MAP_FULL branch, no growMapSize, no retry
        }
```

and its **only** two callers throw the `false` away — `EncodePipeline.cpp:103` and `:111`.

**[probe] actual behaviour at exhaustion:**

```
=== TEST 1: writeRaw with growth room (maxMapSize 64MB) ===
  MAP_FULL hit -> growMapSize() attempt      (x4)
writeRaw: ok=200 fail=0 grow_calls=4

=== TEST 2: writeBatch at mapsize exhaustion (no grow branch) ===
  writeBatch: mdb_put failed: MDB_MAP_FULL: Environment mapsize limit reached
      -> return FALSE, items NOT cleared
writeBatch returned false ; items remaining = 200
```

So, concretely, on mapsize exhaustion:

1. `writeBatch` aborts the whole transaction — **every chunk in the batch is lost, not just the one that
   did not fit**, because they all share one txn.
2. It logs one `spdlog::error` and returns `false`.
3. `EncodePipeline` discards the `false`. The batch vector still holds the entries (the `items.clear()`
   at `LmdbStore.cpp:188` is only reached on the success path) — so the *only* thing preventing
   infinite retry is the trim at `EncodePipeline.cpp:105-108`:

   ```cpp
        if (size_t size = local_palettes.size(); size > 128) {
            local_palettes.resize(std::max(size / 2, static_cast<size_t>(64)));
            local_palettes.shrink_to_fit();
        }
   ```

   which **discards the entries outright, with no log, no error, and no backoff**. The comment on line
   106 (`// подрезаем в 2 раза, надеемся что следующий батч будет меньше` — "trim in half, we hope the
   next batch is smaller") shows the author expected batches to shrink on their own, not that a failed
   write is being eaten.

**Is a mapsize-full write reported anywhere, or does it look like a successful block set?**
It looks like a **silent** drop. The one `spdlog::error` at `LmdbStore.cpp:182` names `mdb_put` but not
the keys, the chunk coords, the batch size, or the mapsize. There is no counter, no metric, and nothing
in the trim path. Note also that the code at `LmdbStore.cpp:18` sets `DEFAULT_MAP_SIZE = 4 GiB` while
`main.cpp:44` passes a cap derived from `--db-max-size-mb` (default 262144 MB = 256 GB) as
`max_map_size` — so `growMapSize` is expected to work for a long time before this fires, which is
exactly why a silent failure here would go unnoticed in staging.

**Fix direction (not applied):** give `writeBatch` the same `MDB_MAP_FULL` → `growMapSize` → retry
branch as `writeRaw`, and honour the `bool` at `EncodePipeline.cpp:103/111` by re-queuing rather than
trimming.

## Finding 3 — `readRawBytes` conflates "not found" with "read failed"  · SEVERITY: high

`src/apps/chunk_store/Storage/disk/LmdbStore.cpp:131-140`

```cpp
    int rc = mdb_get(txn, dbi_, &key_val, &data_val);
    if (rc == MDB_NOTFOUND) {
        mdb_txn_abort(txn);
        return std::nullopt;
    }
    if (rc != 0) {
        spdlog::error("mdb_get failed: {}", mdb_strerror(rc));
        mdb_txn_abort(txn);
        return std::nullopt;        // <-- same value
    }
```

Both branches return `std::nullopt`. The error *is* logged, which is better than nothing, but the
signal is destroyed at the API boundary, and **every caller is written against the conflated
semantics**:

- `ChunkStore.cpp:65-72` (`GetChunk`) — treats `nullopt` as "new chunk", returns an empty chunk. Benign
  in isolation.
- **`ChunkStore.cpp:117-137` (`setBlock`) — the damaging one.** A read error is read as "chunk does not
  exist", so a fresh `MutableChunk` is created, the one block is set, and the partial chunk is
  `cache_.put()` + `markDirty()`ed. The next flush writes back a chunk that is **entirely air except
  the one block just set.** A transient read blip destroys 192 KB of terrain.
- `CASHandler.cpp:23-25` — `nullopt` is reported to the client as `Result::Conflict`, so a disk read
  error presents to the caller as a legitimate CAS conflict.
- `AsyncGetChunk` (`ChunkStore.cpp:242`) — falls into the world-generator path, so a disk error causes
  a **fresh terrain to be generated over the top of a chunk that already exists on disk**, and that
  regenerated chunk is then written back.

**Fix direction:** return `std::expected`/a distinct error variant, or add an out-param carrying the
`mdb_strerror`, so callers can tell a miss from a failure.

## Finding 4 — unchecked `mdb_txn_abort` / `mdb_close` / `mdb_env_close`  · SEVERITY: low

`src/apps/chunk_store/Storage/disk/LmdbStore.cpp:62, 71, 74, 87, 100, 109, 133, 138, 145, 183`

- `mdb_txn_abort(txn)` — return value discarded at all 9 sites above.
- `mdb_close(env_, dbi_)` — line 71, discarded.
- `mdb_env_close(env_)` — line 74, discarded. (This one is `void` in `lmdb.h:753`; the other two are not.)

None of these are `[[nodiscard]]` in `lmdb.h` (checked), so `-Wunused-result` would not catch them, and
in practice `mdb_txn_abort` can only fail on a bad handle — which cannot happen at these call sites
because the handle came from a successful `mdb_txn_begin`. **Correctly low priority: these are inert in
practice and should not be padded into the fix list.** Listed for completeness only.

`mdb_txn_commit(txn)` at **`LmdbStore.cpp:62`** is a real one though — it is inside `open_()`, the DBI-open
bootstrap, and its result is discarded entirely (not even logged). A commit failure there leaves `dbi_`
in an indeterminate state that no later check will detect.

## Finding 5 — `open_()` failure leaves a half-constructed store that stays silent forever  · SEVERITY: high

`src/apps/chunk_store/Storage/disk/LmdbStore.cpp:29-66`, constructor at `:20-23`

```cpp
LmdbStore::LmdbStore(const std::string& db_path, size_t max_map_size)
    : db_path_(db_path), maxMapSize_(max_map_size) {
    open_();                       // returns void — failure is NOT propagated
}
```

`open_()` returns `void` and `return`s early on each `mdb_env_*` failure (lines 33, 37, 41, 45, 54,
and 61/64 for the DBI). The constructor does not check anything, and `main.cpp:44` constructs
`ChunkStore store(db_path, ...)` without validating it. So if `mdb_env_open` fails — **bad path,
permissions, disk full, or a corrupt `data.mdb`** — the daemon starts, `main.cpp:48` reports the TCP
service listening and `:55` reports the router connected, and the process then serves reads and writes
against a null `env_` for the rest of its life.

This is the closest C++ analogue to the `GetRoot`-without-`Verifier` crash class: a startup-time
failure that should abort the process instead degrades into indefinite misbehaviour.

Note also `mdb_env_set_mapsize(env_, DEFAULT_MAP_SIZE)` at line 39 uses the hardcoded 4 GiB constant and
**ignores the `max_map_size` parameter the caller passed in** (it is only used later, by
`growMapSize`). That is a separate smell from the error handling, but it is why Finding 2 is likely to
fire sooner than the `--db-max-size-mb` flag suggests.

## Finding 6 — `HasChunk` swallows every read error as "no"  · SEVERITY: medium

`src/apps/chunk_store/Storage/disk/LmdbStore.cpp:86-88`

```cpp
    int rc = mdb_get(txn, dbi_, &key_val, &data_val);
    mdb_txn_abort(txn);
    return rc == 0;
```

`mdb_txn_begin` is checked (via the macro), but the `mdb_get` result is folded straight into a bool with
no log. A `MDB_BAD_VALSIZE` or a corrupt-page read becomes a clean `false`, i.e. "this chunk is not
stored", which callers use to decide whether to generate new terrain.

## Finding 7 — `FlushPipeline` removes keys from the retry set before attempting the write  · SEVERITY: high

`src/apps/chunk_store/Storage/disk/FlushPipeline.cpp:36-73` (see gp-4ll B2 for the same site from the
caller's side; here it is stated as an LMDB consequence)

`local.swap(dirty_chunks_)` at line 40 clears the retry set up front. On a failed `writeRaw` the key is
never re-inserted, so the chunk is not retried. Line 61 `if (!pinned) continue;` drops it entirely if it
has since been evicted from the cache, also unlogged. The function returns `saved > 0` (line 72), so
even a caller that cared could not distinguish "all saved" from "3 of 40 saved".

**Credit where due:** the `writeRaw` return value *is* checked here (lines 56, 65). This site is not
part of the "discarded return" family; its bug is the missing retry, not a missing check.

---

## Zero-copy claim: `FlatBuffer → LMDB mmap → TCP send buffer`

`AGENTS.md` (CONVENTIONS) states: *"Zero-copy: Chunk data flows FlatBuffer → LMDB mmap → TCP send buffer"*.

**The claim is not accurate, and there is error handling at each of the two hops that gets swallowed.**

Hop 2, `LMDB mmap → TCP send buffer` — **there is no mmap reuse and no zero-copy, and a
copy is made *twice***:

- `LmdbStore.cpp:142-144` — `readRawBytes` copies the LMDB value into a fresh
  `std::vector<uint8_t>` before the txn is aborted. Not zero-copy; the copy is required because
  `mdb_txn_abort` invalidates the mmap pointer.
- `frame.cpp:26-37` (`pack`) and `:39-52` (`pack_router`) — `make_shared<std::vector<uint8_t>>`
  followed by `std::memcpy(frame->data() + 5, data, len)`. A second full copy of every payload, plus a
  third copy in the `FlatBufferBuilder` that produced it (`IoUringChunkStoreService.cpp:120-127`, whose
  `fb` goes out of scope only after `sendResponse` has memcpy'd it).

So the real chain is: encode into a `FlatBufferBuilder` → copy to `std::vector` → copy to frame vector
→ `io_uring_prep_write` (a **kernel** copy, `io_uring_connection.cpp:628-629`). Three user-space copies
plus one kernel-side copy. Whatever the mmap is doing, the data does not travel mmap-to-send-buffer.

Hop 2 error handling — **`send()` cannot report failure by construction.**
`IoUringConnection::send` (`io_uring_connection.cpp:575-583`) and `send_raw` (`:585-595`) both return
`void`. Two failure modes are logged or dropped:

- `io_uring_connection.cpp:618-621` — submission-queue full: `spdlog::warn("SQ full, N writes queued")`
  then `return`, leaving the op in `write_queue_`. Acceptable.
- **`io_uring_connection.cpp:634-635`** — `io_uring_submit(&ring_write_)` return value is **discarded
  with no log at all.** A negative return means the batch was not submitted; the ops stay in
  `in_flight_writes_` and will never complete, because nothing re-drives them. The
  `kWriteBatchLimit` comment at line 18 (`// TODO bug! ... need validate that it good solution and use,
  or find bug`) suggests this area is already suspected.
- `IoUringChunkStoreService.cpp:220-228` `sendResponse` — `if (conn->is_open()) conn->send(...)`, and
  `send` is `void`. **A response to a client is silently discarded whenever the connection has a bad
  `is_open()` read or the send is refused.** Since the write was already `async` and acknowledged
  (Finding C1 in gp-4ll), the client waits for a reply that was never sent and has no timeout story in
  this file.

Hop 1, `FlatBuffer → LMDB` — the read side is where the swallowed error already lives (Findings 3 and 7).

**Verdict on the brief's specific question:** yes, there are errors between the hops, and they are
swallowed. Specifically the `io_uring_submit` result at `io_uring_connection.cpp:635` (unlogged, wedges
writes permanently) and the `sendResponse` no-op at `IoUringChunkStoreService.cpp:226-227` (unlogged,
client never learns).

---

## Ranked, with the concrete data-loss consequence of each

| # | Site | Consequence |
|---|---|---|
| 1 | `LmdbStore.cpp:13-16` (`LMDB_TX_COMMIT`) | commit failure → `return true`; callers report a save that never happened |
| 2 | `LmdbStore.cpp:181-185` + `EncodePipeline.cpp:103,111` | mapsize-full batch loses **every** chunk in the txn, result discarded, retries trimmed away silently |
| 3 | `LmdbStore.cpp:131-140` + `ChunkStore.cpp:117-137` | read error read as "no chunk" → whole 192 KB chunk overwritten with air |
| 4 | `LmdbStore.cpp:20-23, 29-66` | `open_()` failure never propagated; daemon serves from a null env forever |
| 5 | `FlushPipeline.cpp:40, 61` | failed write never retried; the chunk is dropped from the dirty set permanently |
| 6 | `LmdbStore.cpp:86-88` | read error reported as "chunk absent" → duplicate terrain generation |
| 7 | `io_uring_connection.cpp:635` | discarded `io_uring_submit` → write batch never completes |
| 8 | `IoUringChunkStoreService.cpp:226-227` | response silently not sent; client hangs |
| 9 | `EntityStateStorage.cpp:101,142,168,184,207` | all failures logged at `debug` → invisible in production |
| 10 | `LmdbStore.cpp:62,71,74,87,...` | discarded abort/close returns — **inert**, listed only for completeness |

## Correctly handled — stated so the list is not padded

- `writeRaw`'s `MDB_MAP_FULL` → `growMapSize` → retry loop (`LmdbStore.cpp:94-118`) is a good
  implementation. `writeBatch` should copy it.
- `growMapSize` (`:149-171`) checks all three of its returns and gives up cleanly at the cap.
- `readRawBytes`'s `mdb_txn_begin` and the `MDB_NOTFOUND` split (`:123-135`) are right; only the
  exit-value conflation is wrong.
- `EntityStateStorage`'s entire `doLoad`/`doSave`/`doDelete` trio is exemplary control flow — the only
  defect is log severity.
- `IoUringChunkStoreService.cpp:71-78` verifies the FlatBuffer before `GetRoot`.

## Commands run

```bash
grep -rn "mdb_" --include=*.cpp --include=*.h --include=*.hpp src/ | wc -l
grep -rln "mdb_" --include=*.cpp --include=*.h --include=*.hpp src/
grep -rn "writeBatch\|writeRaw\|flushDirtyChunks" --include=*.cpp --include=*.h src/
awk 'NR>=1 && NR<=197' src/apps/chunk_store/Storage/disk/LmdbStore.cpp
awk 'NR>=1 && NR<=212' src/apps/entity_state_store/EntityStateStorage.cpp
awk 'NR>=570 && NR<=700' src/engine/net/src/io_uring_connection.cpp
awk 'NR>=1 && NR<=54'   src/engine/net/src/frame.cpp
g++ -std=gnu++20 -O1 -o probe probe.cpp -I<conan>/src/libraries/liblmdb -L<conan>/build/Debug -llmdb -lpthread
./probe        # see [probe] blocks above
```
