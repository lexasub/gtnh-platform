# Audit gp-4ll — ignored-error return sites in C++ and their blast radius

**Date:** 2026-09-26 · **Scope:** `src/**` C++ (production + tests) · **Mode:** read-only, no source file modified
**Build under audit:** `CMakeLists.txt:12,18` → global `-Wall -Wextra -Wpedantic`, and `-Werror` when `CMAKE_BUILD_TYPE=Release` (confirmed `cmake-build-release/CMakeCache.txt:28` `CMAKE_BUILD_TYPE:STRING=Release`).

## Method

```bash
cd /home/su/src/local/gtnh-platform
grep -rn "mdb_" --include=*.cpp --include=*.h --include=*.hpp src/            # LMDB surface
grep -rn "(void)" --include=*.cpp --include=*.h --include=*.hpp src/           # (void) casts
grep -rn "nodiscard"  --include=*.cpp --include=*.h --include=*.hpp src/        # nodiscard decls (80)
grep -rn "GetRoot<"   --include=*.cpp --include=*.h --include=*.hpp src/        # 150 unverified FB roots
grep -rn "writeBatch\|writeRaw\|flushDirtyChunks" --include=*.cpp --include=*.h src/
```

Cross-referenced every fallible call site against its **caller**, not just the callee, because blast radius
is a property of the call chain, not of the pattern.

## Summary — classified by consequence

| Class | Count | Meaning |
|---|---|---|
| **A — can kill a daemon / leave a writer permanently dead** | **3** | requires a code fix, not a log fix |
| **B — silently loses durable data, looks like success to the caller** | **4** | chunk/entity state vanishes on restart |
| **C — reports a write as successful when it was not** | **2** | false ack on the wire |
| **D — real error converted to a benign default** | **3** | corrupt-but-quiet |
| **E — logged, no state change** | **6** | acceptable |
| **F — inert (pure `void` return, or test-only)** | **~50** | no action |

---

## Class A — can kill a daemon, or wedge a writer thread forever

### A1. `CASHandler::flush()` — a failed write is logged, then the retry queue is destroyed
`src/apps/chunk_store/Storage/CASHandler.cpp:81-85`

```cpp
        if (!lmdb_.writeRaw(key, encoded.data(), encoded.size())) {
            spdlog::error("CASHandler::flush: writeRaw failed for key {}", key);
        }
    }
    pending_.clear();          // <-- line 85: unconditional
    return n_chunks;           // <-- line 86: reports n_chunks, not "wrote n_chunks"
```

Consequence: **`pending_` is the only copy of the CAS change.** `casBlock()` (line 37/49) mutates the
in-memory cache and appends to `pending_`; the durable write happens only here. When `writeRaw` fails
(transient I/O error, `MDB_MAP_FULL` that `growMapSize` could not relieve, disk full), the code logs and
then `clear()`s the queue anyway. The block change is **gone**. The caller is told `n_chunks` chunks were
flushed, so the count is wrong in the same statement that loses the data.

This is the same shape as the bug class that has been productive all week: an error is observed, logged,
and then structurally discarded one line later.

Note also that `flush()` currently has **no production caller** (`grep -rn "cas_\.\|CASHandler::flush"`
returns only the definition and the header). `ChunkStore::casBlock` (ChunkStore.cpp:105) calls
`cas_.casBlock` but nothing ever calls `cas_.flush()`. So today the queued CAS changes are **never
flushed at all** — see note A3.

### A2. `EncodePipeline::encodeLoop()` — `writeBatch` result dropped on both call sites
`src/apps/chunk_store/Storage/EncodePipeline.cpp:103` and `:111`

```cpp
        if (!local_palettes.empty()) [[likely]] {
            lmdb_->writeBatch(local_palettes);     // line 103 — bool discarded
        }
    ...
    if (!local_palettes.empty()) {
        lmdb_->writeBatch(local_palettes);         // line 111 — bool discarded
    }
```

`writeBatch` returns `false` on any `mdb_put` failure (`LmdbStore.cpp:181-185`) and — critically — does
**not** clear `items` on that path. The caller ignores the return, so the batch vector keeps its entries.
Worse, on the next loop iteration `local_palettes` is re-submitted (line 103), and the trimming logic at
line 105-108 (`resize` to half, `shrink_to_fit`) **discards entries outright** with no error path at all.

Net effect: a failed batch is retried a few times and then silently trimmed away. This is the primary
write path for generated/encoded chunks (2 encode threads), so a sustained LMDB failure loses chunk data
with **no log line at all** on the trim path.

### A3. `ChunkStore::casBlock()` queues into a `pending_` that nothing drains
`src/apps/chunk_store/Storage/CASHandler.cpp:9-52` (queue) + no caller of `flush()`

Not a dropped *return value*, but the same consequence class and worth naming because it is the write
path the brief asked me to prioritise: `casBlock` mutates cache + appends to `pending_` and returns
`Result::Ok` to the network layer, which sends `CASStatus_OK` on the wire
(`IoUringChunkStoreService.cpp:197-217`). Because `flush()` is never invoked in production, the caller
is told "OK" and the change is never durable. A successful-looking acknowledgement of an undurable write.

---

## Class B — silently loses durable data while the caller sees success

### B1. `LMDB_TX_COMMIT` — a failed commit logs and falls through to `return true`
`src/apps/chunk_store/Storage/disk/LmdbStore.cpp:13-16` (macro), used at `:113` and `:187`

```cpp
#define LMDB_TX_COMMIT() \
    if (int rc = mdb_txn_commit(txn); rc != 0) [[unlikely]] { \
        spdlog::error("mdb_txn_commit failed: {}", mdb_strerror(rc)); \
    }
```

The macro has no `return`/`else`. At `LmdbStore.cpp:113` (`writeRaw`) and `:187` (`writeBatch`) execution
falls straight through to `return true`. **A commit that failed is reported to the caller as a successful
write.** `writeRaw`'s result feeds `FlushPipeline::flushDirtyChunks` (line 56/65) and
`ChunkStore::SaveChunk` (ChunkStore.cpp:157), which is the value returned to the client's `SaveChunkResp`.

This is the most consequential single macro in the file: it is the difference between "the flush
reported 5 chunks saved" and "5 chunks were lost."

### B2. `FlushPipeline::flushDirtyChunks()` — a failed write leaves the chunk out of the next flush round
`src/apps/chunk_store/Storage/disk/FlushPipeline.cpp:45-72`

```cpp
    std::unordered_set<int64_t> local;
    {
        std::lock_guard lock(dirty_mutex_);
        local.swap(dirty_chunks_);       // line 40 — keys removed from the retry set up front
    }
    ...
        if (palette) {
            if (lmdb_->writeRaw(key, palette->data(), palette->size())) { ++saved; }   // line 56
        } else {
            const MutableChunk* pinned = cache_->getPinned(key);
            if (!pinned) continue;                                                        // line 61
            ...
            if (lmdb_->writeRaw(key, encoded.data(), encoded.size())) [[likely]] { ++saved; }  // line 65
        }
```

The good news, and it deserves saying: **this site does check the return value.** It increments `saved`
only on success, so the count is honest.

The bug is structural. Keys are swapped out of `dirty_chunks_` at line 40 *before* any write is attempted.
On failure the key is never re-inserted, so the chunk is never retried — it is only recovered if something
else marks it dirty again. Line 61 `if (!pinned) continue;` is the same shape: an evicted-from-cache
chunk is dropped from the flush entirely, with no log.

`flushDirtyChunks` returns `saved > 0` (line 72), so a caller cannot distinguish "all saved" from
"some failed" even if it wanted to.

### B3. `ChunkStore::setBlock()` — read failure is indistinguishable from "chunk does not exist"
`src/apps/chunk_store/Storage/ChunkStore.cpp:117-137`

```cpp
    MutableChunk* chunk = const_cast<MutableChunk*>(getCached(cx, cy, cz));
    if (!chunk) {
        auto wire = lmdb_.readRawBytes(key);
        MutableChunk local;
        if (wire) local.fromWire(wire->data(), wire->size());
        local.setBlock(lx, ly, lz, id);
        ...
```

`readRawBytes` returns `std::nullopt` for **both** `MDB_NOTFOUND` and a genuine read error
(`LmdbStore.cpp:132-140` collapses them). Here a transient read failure is treated as "no such chunk":
a fresh `MutableChunk` is built, the single block is set on it, and the result is `cache_.put()` and
`markDirty()`. The **entire rest of the chunk is then overwritten with air** on the next flush, because
the partially-populated `local` is what gets encoded and written back.

This is the worst *consequence* in the whole audit: a read blip destroys a whole 192 KB chunk.

The same `if (wire)` pattern appears at `ChunkStore.cpp:66-72` (`GetChunk`), where it is benign (a
missing chunk legitimately becomes an empty chunk), and at `CASHandler.cpp:23-25`, where it is
misreported as `Result::Conflict`.

### B4. `ChunkStore::GetChunk()` — decode failure returns nullptr, callers turn it into "air"
`src/apps/chunk_store/Storage/ChunkStore.cpp:74-79`, callers at `:86, :92, :98`

```cpp
    if (!chunk->fromWire(wire->data(), wire->size())) {
        delete chunk;
        return nullptr;
    }
```

The nullptr is correct and necessary. But the three public readers each convert it to a **default value
with no log**:

```cpp
uint16_t ChunkStore::GetBlockAt(BlockPos pos) const {
    auto chunk = GetChunk(...);
    if (!chunk) return 0;      // line 86 — indistinguishable from "there is an air block here"
```

`0` is the air block id. So a decode failure on `GetBlockAt` is observationally identical to a chunk
full of air. `GetMeta` (line 92) and `GetMultiblock` (line 98) have the same shape, and `GetMultiblock`
returning 0 is how the SimCore side is told "no multiblock here" — so a decode failure can make the
simulation tear down a multiblock's controller association.

---

## Class C — reports a write as successful when it was not

### C1. `AsyncSetBlock` / `AsyncSaveChunk` — a `true` ack is sent even when nothing was written
`src/apps/chunk_store/Storage/ChunkStore.cpp:180-205`, replies at
`src/apps/chunk_store/Network/IoUringChunkStoreService.cpp:143-155, 173-185`

```cpp
    asio::post(io_pool_, [...]() mutable {
        bool result = false;
        try {
            SetBlock(coord, pos, blockId, meta, mbId);
            result = true;                       // line 186 — unconditional
        } catch (...) { result = false; }
        if (callback) callback(result);
    });
```

`SetBlock` (ChunkStore.cpp:148-152) delegates to `setBlock`, which returns `void`. It only marks the
chunk dirty in the in-memory cache; the LMDB write happens later on the flush thread and its failure is
not connected to this callback. So `result = true` is set regardless of whether the write ever lands.
The `catch (...)` is dead code for write failures — there is no exception path that a `void` function
raises for a failed LMDB put.

The client receives `CreateSetBlockResp(fb, success=true)`. Same for `AsyncSaveChunk` → line 199, which
at least propagates `SaveChunk`'s bool — but that bool is contaminated by **B1** (commit failure returns
true).

### C2. `RouterEventPublisher` — `Publish` is `void`, so a failed publish is invisible
`src/apps/simcore/Network/RouterEventPublisher.cpp:212, 234, 247` and throughout

`router_->Publish(...)` returns nothing, so there is nothing for the compiler to nag about. But the
**router publish path** (the third path the brief asked to prioritise) has a matching hole on the Go
side: `writePublish` (`src/apps/meta_db/router_client.go:340-352`) logs `"[router] publish error"` and
returns `void`. A dropped publish on a save response is silent data loss from the client's point of
view. Cross-referenced in gp-7d1.

---

## Class D — a real error converted to a benign default

### D1. `GetPlayerCount()` — SQL error becomes `0`
`src/apps/meta_db/db.go:332-339`

```go
	err := m.db.QueryRow("SELECT COUNT(*) FROM players").Scan(&count)
	if err != nil {
		return 0
	}
```

A broken database is indistinguishable from "no players have ever logged in." Silent, unlogged.

### D2. `rows.Err()` missing after a `rows.Next()` loop
`src/apps/meta_db/db.go:183-194`

```go
	for rows.Next() {
		...
		if err := rows.Scan(&s.Slot, &blockID, &count); err != nil { return nil, err }
		...
	}
	return result, nil          // line 194 — rows.Err() never checked
```

Iteration can stop early on a mid-stream DB error. This is the Go convention this codebase gets right
elsewhere — `quest_progress.go:51`, `reward_handlers.go:71,102,133,194`, `exchange_handlers.go:124` all
check `rows.Err()`. `db.go` is the outlier. **`GetInventory` is the function the router calls on every
`player.joined`**, so an early stop yields a short inventory that gets published to the client as if it
were the player's real inventory.

### D3. `readRouterFrame` returns `nil` payload for a 1-byte frame — caller treats it as valid
`src/apps/meta_db/router_client.go:271-282`

`totalLen == 0` returns `(mt, nil, nil)`. Callers that expect a payload get `nil` with **no error**. The
topic-length check at `router_client.go:189-195` guards `handlePublish`, but `handleRouterFrame` →
`msgHealthRequest` (line 171) forwards the nil payload straight back to the router.

---

## Class E — logged, no state change (correctly handled; no action)

These are worth listing precisely so the list is not padded with them as if they were findings:

| Site | Why it is fine |
|---|---|
| `LmdbStore.cpp:99-105` `writeRaw` MAP_FULL branch | aborts, grows the map, retries, returns false on give-up. **Correctly handled.** |
| `LmdbStore.cpp:117` `writeRaw` post-resize MAP_FULL | logs and returns false; caller sees failure. Correct. |
| `LmdbStore.cpp:149-171` `growMapSize` | every `mdb_*` return checked. Correct. |
| `LmdbStore.cpp:121-147` `readRawBytes` | txn_begin, mdb_get checked. Only the NOTFOUND/error conflation is at fault (B3). |
| `EntityStateStorage.cpp:99-212` | all 15 `mdb_*` calls checked, commits checked. Best-handled LMDB code in the repo — but all failures are logged at **`spdlog::debug`** (lines 101, 142, 168, 184, 207), which is off in a default `spdlog::info` run. |
| `IoUringChunkStoreService.cpp:71-78` | **has a `flatbuffers::Verifier`** before `GetRoot`. This is the pattern the other 150 `GetRoot` sites are missing. |

---

## Class F — inert

`~50` further sites: `(void)` casts of `void`-returning calls, test-only code, and `[[nodiscard]]`
*accessors* whose value is genuinely optional (`RecipeProgress.h:46-172` component getters,
`MutableChunk.h:47-57` `getBlock`/`getMeta`/`getMultiblock`, `ResourceBufferStateStore.h:50-63`).
Those are fine. See **gp-ejc** for the full `(void)`-cast classification.

---

## Ranked recommendations (not applied — this audit is read-only)

1. **B1** — give `LMDB_TX_COMMIT` a `return false;` (or take a bool out-param). One line, kills the
   worst class of silent data loss.
2. **B3** — split `readRawBytes`' failure modes so a read error is not read as "chunk absent".
3. **A1** — move `pending_.clear()` inside the success branch of the loop.
4. **A2/A3** — honour `writeBatch`'s bool in `EncodePipeline` and re-queue on failure; give `flush()`
   a production caller.
5. **B2** — re-insert failed keys into `dirty_chunks_`.
6. **B4/D1/D2** — log on the `nullptr`/`0`/`rows.Err()` paths.

## Commands run

```bash
git log --oneline -3
grep -rn "mdb_" --include=*.cpp --include=*.h --include=*.hpp src/
grep -rn "(void)" --include=*.cpp --include=*.h --include=*.hpp src/
grep -rn "nodiscard" --include=*.cpp --include=*.h --include=*.hpp src/
grep -rn "GetRoot<" --include=*.cpp --include=*.h --include=*.hpp src/
grep -rn "writeBatch\|writeRaw\|flushDirtyChunks" --include=*.cpp --include=*.h src/
grep -n "Werror\|Wall\|Wextra" CMakeLists.txt; grep -n CMAKE_BUILD_TYPE cmake-build-release/CMakeCache.txt
awk 'NR>=13 && NR<=16' src/apps/chunk_store/Storage/disk/LmdbStore.cpp
```
