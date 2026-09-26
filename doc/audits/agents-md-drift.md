# Audit gp-l2l — `AGENTS.md` claims vs the tree

**Date:** 2026-09-26 · **Commit:** `926521cc` · **Scope:** read-only. `AGENTS.md` is NOT modified.

`AGENTS.md` is stamped `**Generated**: 2026-09-19` (line 3) — 7 days old. That is
short enough to expect drift, and there is a lot of it. Findings are grouped by
severity. §7 lists the claims that are **correct**, which is the larger half.

Two claims the ticket asked me to check do not exist in the file:
`AGENTS.md` never says "65 untested C++ files" and never says "209 TODO points /
111 active tasks". Recorded as such in §6.1 so nobody goes hunting for them.

---

## 1. Wrong binary names — the run list is wrong twice

`AGENTS.md:150-159` ("**Run** (order matters; binary names as built)"):

| # | `AGENTS.md` says | Actual target + on-disk path | Verdict |
|---|---|---|---|
| 1 | `routerd` | ✅ `message_router/CMakeLists.txt:8` `GO_TARGET "routerd"` | ✅ |
| 2 | `chunkd` | ✅ `chunk_store/CMakeLists.txt:71` | ✅ |
| 3 | `entitystated` | ✅ `entity_state_store/CMakeLists.txt:45` | ✅ |
| 4 | `gatewayd` | ✅ `gateway/CMakeLists.txt:43` | ✅ |
| 5 | **`simcored`** | ❌ `simcore/CMakeLists.txt:146` `add_executable(simcored_exec`; `ls cmake-build-debug/src/apps/simcore/` → `simcored_exec` | ❌ **AD-01** |
| 6 | `metadbd` | ✅ `meta_db/CMakeLists.txt:43,52` | ✅ |
| 7 | **`pipenetworkd`** | ❌ `pipe_network/CMakeLists.txt:36` `add_executable(pipe_networkd`; `ls cmake-build-debug/src/apps/pipe_network/` → `pipe_networkd` | ❌ **AD-02** |
| 8 | `gameclientd` | ✅ `game_client/CMakeLists.txt:57` | ✅ |

Note the inversion relative to the gp-awz ticket: **`AGENTS.md` is the wrong document
here, not `run.sh`.** `run.sh:52` (`simcored_exec`) and `run.sh:54` (`pipe_networkd`)
are both correct. Whoever fixes this must edit `AGENTS.md`, and must not "correct"
`run.sh` to match `AGENTS.md`.

The `_exec` suffix is deliberate: `src/apps/simcore/CMakeLists.txt:3` declares
`project(simcored)` and the same file builds a `simcored` static library, so the daemon
target needs disambiguation.

### AD-03 (MEDIUM) — the run list is missing two daemons that exist and are launched

`AGENTS.md:150-159` lists 8 steps. The default build also produces **`reciped`**
(`recipe_manager/CMakeLists.txt:39`, at `cmake-build-debug/src/apps/recipe_manager/reciped`)
and **`pipe_networkd`** is listed but `reciped` is not — yet `run.sh:189` launches
`reciped` unconditionally, between `gatewayd` and `entitystated`. So the documented
startup order does not match the actual one:

- actual (`run.sh:186-193`): routerd → chunkd → gatewayd → **reciped** → entitystated → simcored → metadbd → pipenetworkd
- `AGENTS.md`: routerd → chunkd → entitystated → gatewayd → simcored → metadbd → pipenetworkd → gameclientd

Two differences: `reciped` is missing, and `entitystated`/`gatewayd` are transposed
relative to what `run.sh` actually does.

---

## 2. Counts that drifted

### AD-04 (MEDIUM) — "9 runnable daemons" is correct, but the *reason* it is 9 is not what the doc implies

`AGENTS.md:33` `src/apps/ # 9 runnable daemons (assembly points)` and `AGENTS.md:47`
`src/apps/message_router/`.

Verified: the non-test executables in the default build are
`routerd, chunkd, entitystated, gatewayd, pipe_networkd, simcored_exec, reciped`
(7, via `find cmake-build-debug/src/apps -maxdepth 2 -type f -executable`) plus
`gameclientd` (`cmake-build-debug/bin/`) plus `metadbd` = **9**. **The number is right.**

But `AGENTS.md:52` labels `world_generator/` as "**(library, no binary)**" and
`AGENTS.md:56` labels `spatial_index/` as "**STUB, not built**" — while still counting
them inside the 9-item directory listing. The list of 9 *directories* is not the list of
9 *daemons*. This is a real trap for a reader doing arithmetic on the table, and
`README.md:171` compounds it by saying "10 runnable daemons + 2 stubs" (also wrong, in
the other direction — see `stale-docs.md` SD-21).

### AD-05 (LOW) — "14 .fbs files" is **correct**

`AGENTS.md:56` and `AGENTS.md:117`-adjacent `WHERE TO LOOK` row both say 14.
`ls src/protocol/*.fbs | wc -l` → **14**. Verified twice. No change needed.
(`ROADMAP.md:42` says 12 and is the stale one — see `roadmap-gaps.md`.)

### AD-06 (MEDIUM) — the 192 KB chunk format claim is wrong in the implementation

`AGENTS.md:158`: `Chunk format: 32 KB + 32 KB + 128 KB = 192 KB per chunk`.

The storage layer is palette-encoded, not three flat arrays. `src/apps/chunk_store/Storage/SectionCodec.h:5-15`
documents the on-disk format as `[magic "GCHK"][ver 1][sec_cnt 8] sections[8]: [palette_size u16][palette][bits_per_index u8][indices][meta_count][meta][mb_count][mb]`,
and `SectionCodec.h:20-22` gives `SEC_SZ=16, SEC_VOL=4096, SEC_CNT=8` — 8 sections of
16³, not one flat 32³ triple. The in-memory form is a tagged union with sparse meta and
flat fallbacks (`Storage/cache/MutableSection.h:10,40-45`).

The repo's own measurement: `src/apps/chunk_store/Storage/chunkd_load_test.cpp:96` —
"Each chunk = ~260 bytes palette-encoded".

The same wrong figure appears in 9 places (see `stale-docs.md` SD-20). This is the
single most-repeated stale claim in the repo.

### AD-07 (MEDIUM) — "Multiblock ID stored in meta-layer (O(1) lookup without scanning world)" is no longer accurate

`AGENTS.md:159`. The meta layer still holds `mb_id`, but it is **sparse and sorted, with
a linear-threshold fallback**, not an O(1) array:
`src/apps/chunk_store/Storage/cache/MutableSection.h:37-45` —
`meta_entries`/`mb_entries` are `std::vector<std::pair<...>>` "Sorted by local_idx →
binary search O(log N)", with `SPARSE_THRESHOLD = 256` and `meta_flat`/`mb_flat`
fallbacks. The comment at `MutableSection.h:39` literally says "Threshold 256 → flat
array fallback".

So the lookup is O(log N) for the common case, and the flat fallback — when there are
>256 meta entries in a section — reintroduces the flat array the doc claims not to exist.

### AD-08 (LOW) — the "Spatial queries → STUB" claim is correct

`AGENTS.md:130` and the TODO at `AGENTS.md:194` both say SpatialIndex is a 2-line stub,
not built. Verified:
- `src/apps/spatial_index/main.cpp` is 2 lines: `// Spatial index stub` / `int main() { return 0; }`
- `CMakeLists.txt:84` has `#add_subdirectory(src/apps/spatial_index)` commented out

**This claim is accurate.** Listed here only because the surrounding docs
(`README.md:71-76` diagram, `doc/c4/level2-container.puml:40`) draw it as a real peer.

### AD-09 (LOW) — "Validation: C++ (not in default build)" is correct, and more so than the doc realises

`AGENTS.md:58` says "Validation | `src/apps/validation/` | C++ (not in default build)".
Verified: there is **no** `add_subdirectory(src/apps/validation ...)` line anywhere in
`CMakeLists.txt` — not even a commented one, unlike `spatial_index` at line 84. The
target exists (`src/apps/validation/CMakeLists.txt:5` `add_executable(validationd`) but
nothing ever configures it. `doc/c4/level2-container.puml:56` calls it a
"C++ (static lib)" — also wrong, it is an *executable*, and it is not built as either.

### AD-10 (LOW) — "Crafting recipes: 15 files" is correct; the .bak files make counting non-obvious

`AGENTS.md:117` says 15. `ls src/content/data/recipes/*.yaml | wc -l` → **15**.
The directory also holds `compressor.yaml.bak` and `mixer.yaml.bak`, which a naive
`ls | wc -l` would count as 17. The claim is right. (`README.md:205` says 14 — that is
the wrong one.)

---

## 3. Library-decision tables

`AGENTS.md:176-215` ("LIBRARY DECISIONS"). Audited row by row.

### AD-11 (HIGH) — `IExternalLogic` exists in no code, only in three README copies of the same claim

`AGENTS.md:171` (CONVENTIONS): "**Language boundaries**: Hot path = C++ only. Sidecars = Go/Python via `IExternalLogic`"

```
$ grep -rln "IExternalLogic" --include=*.h --include=*.cpp src/
(no output)
```

Zero hits in code. There is no `IExternalLogic` interface, no header, no
implementation — the only three occurrences in the tree are the identical claim copied
into `src/apps/chunk_store/README.md:19`, `src/apps/world_generator/README.md:36` and
`src/apps/game_client/README.md:36`, so the same untrue sentence appears in four
documents rather than one. The `src/engine/storage/` directory that would host it
contains only `IEntityStateStorage.h`, `IPlayerInventoryStorage.h` and a
`CMakeLists.txt`.

This is the most misleading line in `AGENTS.md`: it describes a plugin/extension seam
that was never built, and a reader designing a sidecar would look for an interface
that does not exist. **Python is not in the tree at all** — there is no Python sidecar
mechanism, only the legacy note in `AGENTS.md:169` that root `data/` and `src/data/`
are "legacy/tooling only".

### AD-12 (MEDIUM) — the C++ library table omits TBB, liburing, yaml-cpp, nlohmann_json, imgui, lodepng

`AGENTS.md:178-190` lists 10 libraries: Asio, FlatBuffers, EnTT, LMDB, fastnoise2, GLM,
spdlog/fmt, bgfx, GLFW, miniaudio. `conanfile.txt` requires 15:

```
asio/1.32.0, entt/3.16.0, flatbuffers/25.9.23, fmt/12.1.0, glfw/3.4, glm/1.0.1,
miniaudio/0.11.22, nlohmann_json/3.11.3, yaml-cpp/0.8.0, spdlog/1.17.0,
fastnoise2/1.1.1, sqlite3/3.49.1, liburing/2.13, lmdb/0.9.32, imgui/1.91.8,
lodepng/cci.20230410
```

Missing from the table, all of them load-bearing:
- **liburing** — the entire async story. `src/engine/net/include/gtnh/net/io_uring_context.h`,
  `router_client.h`, `gateway.h`, `chunk_store/Network/IoUring*.h`, `simcore/Network/clients/IoUringChunkClient.h`.
  `AGENTS.md:113`-equivalent claim in README calls io_uring "the primary async backend",
  but `AGENTS.md` never names the library.
- **sqlite3** — `CMakeLists.txt` does not `find_package(SQLite3)`, but both
  `recipe_manager/CMakeLists.txt:11` and `simcore/CMakeLists.txt:18` do.
- **yaml-cpp** — machine registry and all YAML recipe loading.
- **nlohmann_json** — `CMakeLists.txt:59` `find_package(nlohmann_json REQUIRED)`.
- **imgui** — `CMakeLists.txt:74` `find_package(imgui CONFIG REQUIRED)`.
- **TBB** — `CMakeLists.txt:58` `find_package(TBB REQUIRED)`; linked in
  `game_client/CMakeLists.txt:398`, `RenderLib/CMakeLists.txt:37`, `game/client/World/CMakeLists.txt:65`.

The table is presented as the project's dependency decision record; it is missing ~40%
of the decisions.

### AD-13 (MEDIUM) — "fastnoise2 | Terrain generation" is right but the C4 diagram says "FastNoiseLite"

`AGENTS.md:183` says fastnoise2 (correct — `conanfile.txt:12`, and
`src/apps/world_generator/OreGenerator.cpp:3` `#include "FastNoise/FastNoise.h"`).
`doc/c4/level2-container.puml:33` says "C++ (FastNoiseLite)" and
`level3-tiny-services.puml:16` says "FastNoiseLite". The diagram is wrong
(FastNoiseLite is a different, single-header library). See `c4-drift.md`.

### AD-14 (MEDIUM) — the Go library table is wrong in one place and thin in another

`AGENTS.md:203-207` lists `net`, `database/sql`, `mattn/go-sqlite3`, FlatBuffers Go.

- ✅ `mattn/go-sqlite3` verified at `src/apps/meta_db/go.mod`.
- ✅ FlatBuffers Go verified — `src/apps/meta_db/go.mod` requires
  `github.com/google/flatbuffers v24.3.25`.
- ❌ **The `replace` directives are not documented at all**, and they are load-bearing:
  both `src/apps/meta_db/go.mod` and `src/apps/message_router/go.mod` have
  `replace github.com/gtnh/platform/gtnh-common => ../../libs/gtnh-common-go`.
  A reader copying the `go.mod` blocks from the doc will get a build failure, and the
  shared Go library `src/libs/gtnh-common-go` appears nowhere in the STRUCTURE tree
  (`AGENTS.md:29-57`) or the WHERE TO LOOK table.
- Also unlisted: `message_router/go.sum` is a **0-byte file** and `dummy.go` is a
  174-byte placeholder — neither is mentioned.

### AD-15 (LOW) — the "What's NOT used" table is correct

`AGENTS.md:209-215` claims no gRPC, no ZeroMQ, no RocksDB, no JSON in Gateway.
Verified: `grep -rn "grpc\|zmq\|zeromq\|rocksdb"` over `src/` and `conanfile.txt` → no
hits. "JSON parsing in Gateway — forbidden" is also consistent with
`src/apps/gateway/` having no JSON dependency. **This section is accurate.**

### AD-16 (LOW) — "spdlog/fmt" is correct; `fmt/12.1.0` is a direct Conan require (`conanfile.txt:5`) and is not mentioned separately

Minor. `AGENTS.md:186` writes them as one row "spdlog/fmt" which is defensible. No
action beyond noting `fmt` is a first-class direct dependency.

---

## 4. `WHERE TO LOOK` and structure claims

### AD-17 (MEDIUM) — `Spatial queries | src/apps/spatial_index/ | STUB` is correct, but the row is a duplicate of the services table and creates a contradiction with C4

Already covered by AD-08. The specific drift: `AGENTS.md:141` says
"`src/apps/spatial_index/` | STUB — not implemented, not built", which **contradicts**
`doc/c4/level2-container.puml:40`, which draws SpatialIndex as
`Container(spatial, "SpatialIndex", "C++ (Boost.Geometry R-tree)", "… O(log n) AABB queries", $tags="planned")`
with a live relationship `Rel(spatial, chunkstore, "R-tree queries: GetMultiblocksIntersecting", $tags="tag_planned")`
(`level2-container.puml:120`). `AGENTS.md` is right and the diagram is wrong. See `c4-drift.md`.

### AD-18 (LOW) — every directory in the STRUCTURE tree exists (verified)

```
$ for p in src/engine/net src/engine/registry src/engine/sim src/engine/storage \
           src/engine/utils src/game/machines src/game/mining src/game/quests \
           src/game/recipes src/common src/content src/protocol \
           src/apps/spatial_index src/apps/validation; do ... done
OK  (all 14)
```

**The structure tree is accurate** as far as it goes. The problem is omissions, not
wrong paths. `src/engine/` has 5 documented subdirs; `ls src/engine/` also has `sim`
and others that are documented. `src/game/` documents 4 (`machines`, `mining`, `quests`,
`recipes`) but `ls src/game/` shows more (`actions`, `client`, `crafting`, `storage`,
`world`, `scenario` — all with their own `CMakeLists.txt` and test targets). So the
tree **understates** `src/game/` by 6 directories, 5 of which have test targets
(`actions/CMakeLists.txt:62,103,143`, `crafting/CMakeLists.txt:45,75`,
`storage/CMakeLists.txt:52,94,138,173,204`, `world/CMakeLists.txt:33,57,82,111`).

*(Related open ticket: `gp-z3r` "Update AGENTS.md structure table to match the current tree".)*

### AD-19 (LOW) — "Game client | bgfx render, ImGui, input, physics" is correct

`AGENTS.md:50`. bgfx and imgui are present; `src/apps/game_client/CMakeLists.txt:57`
builds `gameclientd`, `:398` links TBB. Accurate.

### AD-20 (LOW) — `AGENTS.md:169` "Real content data root = `src/content/data/`" is correct

Verified: `ls src/content/data/` → `recipes/`, `registry/`, `textures/`. And root
`data/` and `src/data/` — `ls -d data src/data 2>&1` to confirm they are legacy. The
claim is consistent with `recipe_manager/main.cpp:60` loading
`dataDir + "/src/content/data/registry/items.csv"`.

### AD-21 (MEDIUM) — the header comment on `AGENTS.md:150` contradicts the table immediately below it

`AGENTS.md:150` says "binary names as built", and then lists `simcored` and
`pipenetworkd`, neither of which is a name as built (AD-01, AD-02). The header makes
the error load-bearing: it tells the reader the list was verified against the build.

---

## 5. TODO list verification

`AGENTS.md:192-200`, the seven unchecked TODO items:

| TODO | Verified state | Verdict |
|------|----------------|---------|
| Pause menu / settings window | `find src -iname "*Pause*" -o -iname "*Settings*"` → no hits | ✅ **still open** |
| Sound: miniaudio in conanfile, no audio code | `grep -rln "miniaudio\|ma_device" --include=*.cpp --include=*.h src/` → **no hits**; `conanfile.txt:8` has `miniaudio/0.11.22` | ✅ **still open** |
| SpatialIndex R-tree/Octree | 2-line stub, `CMakeLists.txt:84` commented | ✅ **still open** |
| Dedicated Drill UI window | `find src -iname "*Drill*"` → only `ElectricDrillHandler`, `DrillComponent.h`, `DrillSystem.{cpp,h}` — **no UI window** | ✅ **still open** |
| Resolve GatewayMsg vs GatewayPayload divergence | `src/protocol/gateway.fbs:34-54` — union stops at `QuestCompletedNotification = 21`, with **gaps** (17, 18 missing) while `GatewayMsg.h:73` reaches 51 | ✅ **still open**, and worse than described |
| Server-authoritative grid state via TileEntityStore RPC | `tile_entity_store.fbs` exists; no beads task implementation landed | ✅ **still open** |
| Refresh stale root README.md and run.sh | ✅ confirmed — this is gp-awz, 25 findings | ✅ **still open** |

All seven TODOs are accurate. The TODO list is the **most reliable part of the file**,
which is a useful signal: the drift is concentrated in the generated STRUCTURE /
SERVICES / LIBRARY sections, not in the hand-maintained prose.

Note on the 5th item: `gateway.fbs`'s `GatewayPayload` union (line 34) is not merely
"stale" — it is **internally inconsistent**, skipping 17 and 18 between
`SetMachineSlotResp = 16` and `QuestProgressUpdate = 19`. `AGENTS.md:112` does correctly
flag the union as stale, and `doc/c4/README.md:110-111` does too, and open tickets
`gp-a6l` / `gp-ocjg` / `gp-48he` / `gp-k8ap` track it.

---

## 6. Claims that could not be verified (recorded so nobody re-chases them)

### 6.1 Not present in the file

The ticket asked me to verify a "65 untested C++ files" figure and a "209 TODO points /
111 active tasks" figure. Neither string exists in `AGENTS.md`:

```
$ grep -n "65 untested\|209 TODO\|111 active\|TODO points\|active tasks" AGENTS.md
(no output)
```

For reference, the actual current numbers (a future audit may want them):

```
$ find src -name "*.cpp" -not -path "*/test*" | wc -l     → 189
$ find src -name "*.h"   -not -path "*/test*" | wc -l     → 277
$ grep -rn "TODO\|FIXME" --include=*.cpp --include=*.h src/ \
    | grep -v "/test\|test_\|_test" | wc -l                →  37
$ bd list --status open | tail -1                          →  Total: 128 issues (128 open, 0 in progress)
```

So the test-coverage TODO density is now **37** in-source markers, and the tracker holds
**128 open issues** across 15 P1 / ~80 P2 / ~35 P3. If a coverage number is wanted in
`AGENTS.md`, it must be recomputed, not back-derived from the ticket.

### 6.2 Verified CORRECT — do not change these

1. **`AGENTS.md:56` "14 .fbs"** — `ls src/protocol/*.fbs | wc -l` → 14. ✅
2. **`AGENTS.md:117` "15 recipe YAML files"** — `ls src/content/data/recipes/*.yaml | wc -l` → 15. ✅
3. **All 14 STRUCTURE-tree directories exist** (AD-18). ✅
4. **The 7 real ports** (4000, 5001, 5005, 5006, 5200, 7777, 7778) — each traced to a
   `main()` default; see `stale-docs.md` §3. ✅
5. **`AGENTS.md:112` "wire protocol = C++ `GatewayMsg` constants (1–51, 1-based)"** —
   `src/common/GatewayMsg.h:73` `kServiceHealthResp = 51`. ✅ (`README.md:114` says 41 and is wrong.)
6. **"What's NOT used" table** (gRPC/ZeroMQ/RocksDB/JSON-in-Gateway) — zero hits. ✅ (AD-15)
7. **All 7 TODO items are still genuinely open** (§5). ✅
8. **`AGENTS.md:88` `docs/gateway-headless-client.md`** — exists (`docs/` with an *s*).
   ✅
9. **`AGENTS.md:169` content data root** — `src/content/data/` ✅
10. **"SpatialIndex STUB"** and **"Validation not in default build"** — both accurate. ✅

---

## 7. The `AGENTS.md:172` self-warning, and why it does not excuse the drift

`AGENTS.md:172` says:

> Note: root `README.md` and `run.sh` still reference the old `src/services/*` layout
> and are stale; trust the source tree and this file.

Two problems:

1. **It points the reader at itself as the fallback authority.** This audit is the proof
   that `AGENTS.md` is not currently a safe fallback: it has two wrong binary names
   (AD-01, AD-02), a fabricated interface (AD-11), and a repeated-in-9-files wrong
   chunk format (AD-06). "Trust this file" is not a safe instruction while it is
   wrong in the same class of thing it is warning about.
2. **The warning is now half obsolete.** `README.md` has *partly* self-corrected —
   `README.md:151-158` and `README.md:171-183` use `src/apps/*`, not `src/services/*`.
   The `src/services/*` layout survives only in `ROADMAP.md:36,206,274,625-631,664`
   (see `roadmap-gaps.md` RG-01).

Suggested rewrite: replace the "trust this file" framing with an explicit
known-errors list pointing at `doc/audits/agents-md-drift.md`, until the findings above
are fixed.

---

## 8. Summary

| ID | Severity | Finding |
|----|----------|---------|
| AD-01 | HIGH | `simcored` in the run list; real target is `simcored_exec` (`simcore/CMakeLists.txt:146`) |
| AD-02 | HIGH | `pipenetworkd` in the run list; real target is `pipe_networkd` (`pipe_network/CMakeLists.txt:36`) |
| AD-11 | HIGH | `IExternalLogic` does not exist anywhere in `src/` — 0 grep hits |
| AD-03 | MEDIUM | Run list missing `reciped`; `entitystated`/`gatewayd` transposed vs `run.sh:186-193` |
| AD-04 | MEDIUM | "9 runnable daemons" is right, but the directory list includes 2 non-daemons |
| AD-06 | MEDIUM | 192 KB chunk format wrong; actual is palette-encoded, ~260 bytes (`SectionCodec.h:5-22`, `chunkd_load_test.cpp:96`) |
| AD-07 | MEDIUM | mb_id lookup is O(log N) sparse + 256-threshold flat fallback, not O(1) (`MutableSection.h:37-45`) |
| AD-12 | MEDIUM | Library table omits liburing, sqlite3, yaml-cpp, nlohmann_json, imgui, TBB |
| AD-14 | MEDIUM | Go `replace` directives and `src/libs/gtnh-common-go` undocumented |
| AD-21 | MEDIUM | `AGENTS.md:150` header says "binary names as built" above a list that is not |
| AD-05 | LOW | ✅ correct — 14 `.fbs` |
| AD-08 | LOW | ✅ correct — SpatialIndex stub |
| AD-09 | LOW | ✅ correct — Validation not built (and *never* `add_subdirectory`d at all) |
| AD-10 | LOW | ✅ correct — 15 recipe YAMLs |
| AD-13 | LOW | fastnoise2 correct here; the C4 diagram's "FastNoiseLite" is the wrong one |
| AD-15 | LOW | ✅ correct — "What's NOT used" table |
| AD-18 | LOW | Structure tree paths all exist, but omits 6 `src/game/` dirs that have test targets |
| AD-19 | LOW | ✅ correct — GameClient description |
| AD-20 | LOW | ✅ correct — content data root |
| AD-17 | LOW | Correct, but `AGENTS.md:141` contradicts `doc/c4/level2-container.puml:40,120` |
| AD-16 | LOW | `fmt` is a first-class direct require, merged into the spdlog row |

**4 HIGH/MEDIUM-HIGH, 6 MEDIUM, 10 LOW** — of which 7 are confirmations that the claim
is correct. `AGENTS.md` is **not** safe to update blindly, but its TODO list and its
directory structure are trustworthy; the generated count/service/library sections are not.
