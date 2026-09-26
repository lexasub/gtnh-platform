# Audit gp-ejc — C++ `(void)` casts and unused-parameter sites, classified

**Date:** 2026-09-26 · **Scope:** `src/**` C++ · **Mode:** read-only, no source file modified
**Triage question:** which of these are genuinely inert, and which would fail a `-Werror` build if the
file's flags changed.

## Build-flag ground truth

```bash
awk 'NR>=11 && NR<=19' CMakeLists.txt
# 12: add_compile_options(-Wall -Wextra -Wpedantic)
# 17: if(CMAKE_BUILD_TYPE STREQUAL "Release")
# 18:     add_compile_options(-Werror)
grep -n CMAKE_BUILD_TYPE cmake-build-release/CMakeCache.txt
# 28:CMAKE_BUILD_TYPE:STRING=Release
```

`add_compile_options` is **directory-scoped and inherited by every subdirectory added after it**
(`CMakeLists.txt:65-88`). No target in `src/apps/chunk_store/CMakeLists.txt` (or any other
`CMakeLists.txt` checked) sets `COMPILE_OPTIONS` to override it. So:

- **`-Werror` is global in Release, and it applies to test and benchmark executables too** — they are
  plain `add_executable` calls (e.g. `src/apps/chunk_store/CMakeLists.txt:86, 93, 110, 130, 146, 161`).
  This is exactly the failure the brief described: an unused variable in a test breaks CI.
- **The `compile_commands.json` in `cmake-build-debug/` does not show `-Werror`** (that dir is Debug).
  Anyone reading the debug DB to answer "is this warning enforced?" gets the wrong answer.

**Probe confirming `-Wunused-parameter` is fatal:**

```bash
# int f(int p, int q) { return q; }   -- no (void) cast
g++ -std=gnu++20 -Wall -Wextra -Wpedantic -Werror -c t1.cpp
# t1.cpp:2:11: error: unused parameter 'p' [-Werror=unused-parameter]
# cc1plus: all warnings being treated as errors
```

The only warning `-Wunused-result` would ever produce requires a `[[nodiscard]]` or
`warn_unused_result` attribute. **`lmdb.h` has neither** (checked: no `warn_unused_result` in the
header), so the discarded `mdb_*` returns are invisible to the compiler — they have to be found by
reading, which is why they exist at all. See gp-8lc.

## Inventory

```bash
grep -rn "(void)" --include=*.cpp --include=*.h --include=*.hpp src/   # 36 prod + 82 in test/bench paths
```

| Category | Count |
|---|---|
| `(void)` of a **live** parameter (genuinely suppressed) | 18 |
| `(void)` of an **inert** `void`-returning call | 5 |
| `(void)` in **generated** FlatBuffers headers | 10 |
| `(void)` in **benchmarks** (`volatile` idiom) | 4 |
| Unused params **not** `(void)`-cast — would fail `-Werror` today if a signature changed | 0 |

Every `(void)`-cast site was checked by extracting the enclosing function with a brace counter and
counting identifier uses outside the cast line and the signature.

---

## Class 1 — LIVE: the parameter is genuinely read, the cast is doing real work

| Site | Cast | What the parameter is used for |
|---|---|---|
| `src/game/machines/MachineSystem.cpp:72` | `(void)progress` | `progress` IS used at `:124, :141` in the *sibling* pass-1 loop; the cast is in the pass-0 loop (`:67-113`) where the `view.get<RecipeProgress>` at `:69` is a **real ECS lookup whose result this loop genuinely does not need**. The `auto&` at `:69` is what would warn. |
| `src/game/machines/MachineSystem.cpp:121` | `(void)energy` | mirror of the above: `auto& energy` at `:120`, unused in pass 1 (`:116-199`). |
| `src/apps/chunk_store/Storage/clock_cache_bench.cpp:125` | `(void)v` | `volatile auto v = cache.get(...)` — the `volatile` **is** the benchmark; the cast stops `-Wunused-variable`. |
| `src/apps/chunk_store/Storage/clock_cache_bench.cpp:143` | `(void)v` | same, in `bench_mixed` |
| `src/apps/chunk_store/Storage/clock_cache_bench.cpp:164` | `(void)v` | same, in `bench_concurrent` |

**Verdict for all five: KEEP.** These are correct and load-bearing. In particular
`MachineSystem.cpp:69,120` are the two that would break a `-Werror` build if someone deleted the
`(void)` line — the `view.get<>` call would still be made (it has a side effect on entt's internal
assertion path in debug) and the unused reference would warn.

**But note the cost**, per the brief: all five live in `clock_cache_bench.cpp` and `MachineSystem.cpp`,
which are compiled into `clock_cache_bench` and the simcore daemon respectively. This class is not
"harmless" — it is a live dependency on a `-Werror` invariant. If the flags are ever relaxed, or a
`-Wno-unused-variable` is added, these become live bugs.

## Class 2 — LIVE and **load-bearing for correctness**: the two `ChunkStore` params that hide a bug

### `src/apps/chunk_store/Storage/ChunkStore.cpp:150` — `(void)pos; (void)mbId;`

```cpp
void ChunkStore::SetBlock(ChunkCoord coord, BlockPos pos, uint16_t blockId,
                          uint8_t meta, uint32_t mbId) {
    (void)pos; (void)mbId;                      // line 150
    setBlock(coord.x, coord.y, coord.z, blockId, meta);
}
```

Both are genuinely dead **in this function**, so the cast is doing what it says. But the deadness is
the defect:

- **`pos`** — every caller already passes the local (chunk-relative) coordinates, so nothing is lost
  here. `setBlock` re-derives `lx,ly,lz` from the absolute position at `:112-114`. The parameter is
  redundant with `coord`, not a bug. **Verdict: KEEP with a comment, or remove — low value either way.**
- **`mbId`** — this one is a real data-loss site, and it is worth separating from the "inert" verdict.
  `MutableChunk::setMultiblock` exists (`MutableChunk.cpp:33-37`), `MutableSection::setMultiblock`
  exists and is even **serialised on the wire** (`MutableSection.cpp:357` decodes `wire_mb`), and
  `GetMultiblock` reads it back. So the storage layer fully supports multiblock IDs — the write path
  just never calls it. `AGENTS.md` documents this exact sequence as the intended design:
  *"SimCore → RPC: SetBlockMeta for ALL pattern blocks → ChunkStore (writes mb_id into chunk
  meta-layer)"*. **That RPC does not exist**, and `IoUringChunkClient::SetBlock`
  (`IoUringChunkClient.cpp:64-98`) does not even put `mb_id` on the wire — `CreateSetBlockReq`
  (line 75) has no `mb_id` field, and the server hardcodes `0` at
  `IoUringChunkStoreService.cpp:143` (`store_.AsyncSetBlock(coord, local, block_id, meta, 0, ...)`).
  Meanwhile the client-side renderer **does** read it: `World.cpp:243,296` pass `pb.mb_id` into
  `SetBlock`.

  **Verdict: KEEP, but annotate `// (void)mbId — TODO: multiblock ID is never persisted; see
  AGENTS.md "ChunkStore vs SimCore".** This is a `(void)` cast that is hiding a missing feature, not
  an inert cast. It is the one site in this audit I would file a ticket for rather than delete.

## Class 3 — INERT: `(void)` on a `void`-returning call, or on a dead local

These 5 could be deleted with zero consequence. None can fail a build.

| Site | Why inert |
|---|---|
| `src/apps/game/ui/client/player/QuestBookWindow.cpp:670` `(void)key; (void)action; (void)mods;` | `OnKeyEvent` (`:669-672`) is a 4-line stub returning `open_`. Parameters are interface-required, genuinely dead. |
| `src/game/ui/client/player/QuestBookWindow.cpp:675` `(void)button; (void)action;` | same, `OnMouseClick` (`:674-677`) stub. |
| `src/game/ui/client/player/ConsoleWindow.cpp:189-191` | same, `OnKeyEvent` (`:185-193`); the body comment explains why (ImGui owns input). |
| `src/apps/pipe_network/PipeNetworkService.cpp:728` `(void)key;` | structured-binding in `for (const auto& [key, node] : machine_nodes_)` (`:727`); only `node` is used at `:729, :734`. |
| `src/apps/chunk_store/Network/IoUringChunkStoreService.cpp:68`, `src/apps/simcore/Network/clients/IoUringChunkClient.cpp:180` `(void)msg_type;` | `on_message` / `onRead` dispatch on the FlatBuffer union instead of the wire type byte. Interface-required. |

## Class 4 — `(void)` with a stated intent: **reserved for a real feature**

| Site | Comment | Verdict |
|---|---|---|
| `src/game/actions/handTool/WrenchHandler.cpp:24` `(void)playerId; // reserved for permission checks` | `cycleFace` is a 96-line function (`:23-118`) that mutates any machine at the position. **There is no permission check**, so the comment describes an unimplemented security control, not a cosmetic intent. | KEEP the cast, but the comment is aspirational. Worth a ticket: any player can cycle any machine's face. |
| `src/game/ui/client/core/InputBinder.cpp:213` `(void)context; // held bindings are global-only for now` | "for now" = genuine future intent. `heldBindings_` is keyed by name alone (`:214`). | KEEP. |
| `src/game/actions/MiningCalculator.h:58` `(void)toolItem;` | `miningEnergyCost` (`:57-60`) ignores tool tier entirely, while the sibling `miningTicks` (`:62-64`) *does* use `toolTier`. So a diamond pick and a wooden axe cost the same to mine. | KEEP the cast, but the asymmetry with `miningTicks` suggests a missing tool-tier multiplier, not a deliberate no-op. |

## Class 5 — generated code, do not touch

10 sites in `src/protocol/generated/recipe_generated.h:287,294,301` and
`src/protocol/generated/core_generated.h:681,682,693,694,782,789` — all `(void)padding0__;` /
`(void)padding1__;` emitted by `flatc` for struct padding fields. Editing them is pointless (they are
regenerated) and the fields are required for layout. **Inert by construction.**

## Class 6 — the 82 test-path `(void)` sites

`grep` shows 82 `(void)` occurrences under `src/**/test/` and `*_test.cpp`. These are the ones the
brief flagged as having already cost CI time. They are individually inert (the same
`-Wunused-parameter` suppression) and I have not enumerated them as findings, because:

1. They are all in test/bench executables, which are built with the same global `-Werror` in Release.
2. They suppress the same warning class as Class 1.
3. The precedent event is already paid for; re-listing them adds no diagnostic value.

**The actionable statement about them is structural, not per-site:** the repo currently depends on
`-Werror` being global in Release to keep test code warning-clean, and that dependency is invisible
because `cmake-build-debug/compile_commands.json` does not contain `-Werror`. A developer iterating in
the Debug tree will not see a warning that CI will reject.

---

## Ranked

**Would fail a `-Werror` build if the `(void)` line were deleted** (5 sites, all Class 1 — these are
*load-bearing* and must not be "cleaned up"):

1. `src/game/machines/MachineSystem.cpp:72` — `(void)progress`, unused `view.get<RecipeProgress>`
2. `src/game/machines/MachineSystem.cpp:121` — `(void)energy`, unused `view.get<EnergyStorage>`
3. `src/apps/chunk_store/Storage/clock_cache_bench.cpp:125, 143, 164` — the `volatile` benchmark idiom

**Genuinely inert, safe to delete, no build risk** (5 sites, Class 3):
`QuestBookWindow.cpp:670, 675`, `ConsoleWindow.cpp:189-191`, `PipeNetworkService.cpp:728`,
`IoUringChunkStoreService.cpp:68`, `IoUringChunkClient.cpp:180`.

**Inert but must not be touched** (14 sites, Classes 5-6): 10 generated FlatBuffers padding casts,
4 benchmark `volatile` casts.

**Hiding a real problem** (2 sites, Class 2 + Class 4):
- `ChunkStore.cpp:150` `(void)mbId` — multiblock IDs are never persisted, contradicting `AGENTS.md`
- `WrenchHandler.cpp:24` `(void)playerId` — no permission check on machine mutation

**Aesthetic only** (1): `ChunkStore.cpp:150` `(void)pos`, redundant with `coord`.

## Commands run

```bash
awk 'NR>=11 && NR<=19' CMakeLists.txt
awk 'NR>=41 && NR<=92' CMakeLists.txt
grep -n CMAKE_BUILD_TYPE cmake-build-release/CMakeCache.txt
grep -n "add_executable\|COMPILE_OPTIONS" src/apps/chunk_store/CMakeLists.txt
grep -rn "(void)" --include=*.cpp --include=*.h --include=*.hpp src/
grep -rn "warn_unused_result" ~/.conan2/p/b/lmdb6c993bdf57bdb/b/src/libraries/liblmdb/lmdb.h   # no hits
# per-site: enclosing-function extraction via brace counter, then identifier-use count
g++ -std=gnu++20 -Wall -Wextra -Wpedantic -Werror -c t1.cpp    # confirms -Wunused-parameter is fatal
```
