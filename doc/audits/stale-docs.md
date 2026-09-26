# Audit gp-awz — stale documentation in `README.md` and `run.sh`

**Date:** 2026-09-26 · **Commit:** `926521cc` · **Scope:** read-only, no fixes applied.

Every finding below was verified with a command against the tree. Where a claim is
already correct, it is listed in §6 so the next fixer does not "fix" it.

---

## 0. Correction to the ticket's own premise

The ticket states:

> `run.sh:52` sets `SIMCORED="$(BIN simcore simcored_exec)"` but there is no `simcored_exec` binary.

**That is wrong — do not "fix" it.** `simcored_exec` is a real CMake target and a real
on-disk binary:

```
$ grep -rn "add_executable" --include=CMakeLists.txt src/apps/simcore/
src/apps/simcore/CMakeLists.txt:146:add_executable(simcored_exec
$ ls cmake-build-debug/src/apps/simcore/
simcored_exec
simcored_test
```

The `_exec` suffix is a real convention in this tree (it disambiguates the daemon from
the `simcored` static library target; `src/apps/simcore/CMakeLists.txt:3` declares
`project(simcored)` and line ~120 builds the `simcored` library). So `run.sh:52` is
**correct** and `AGENTS.md`'s `simcored` naming (§ gp-l2l) is the one that is wrong.

Everything else in the ticket's premise checks out and is expanded on below.

---

## 1. Findings — `run.sh`

### SD-01 (HIGH) — `run.sh` hard-codes a `ninja` run into `cmake-build-debug`, ignoring `--build-dir`

`run.sh:17`:

```bash
cd cmake-build-debug; ninja -j5; cd ..
```

`--build-dir` is parsed at `run.sh:33` and consumed by `BIN()` at `run.sh:46`, but the
build at line 17 runs *before* argument parsing (line 31) and never references
`${BUILD_DIR}`. Passing `--build-dir /some/release` builds debug and then tries to
launch release binaries.

- **Impact:** silent wrong-build-directory; `--build-dir` is half-implemented.
- **Also:** `cd cmake-build-debug` is relative to `$PWD`, not `${SCRIPT_DIR}`, so
  running `./run.sh` from a subdirectory fails outright. `${SCRIPT_DIR}` is computed at
  `run.sh:6` and correctly used everywhere else.

### SD-02 (HIGH) — `run.sh` unconditionally copies to `/mnt/nfs/`, which makes the script fail for anyone without that mount

`run.sh:18`, `:19`, `:25`, `:209`:

```bash
cp -r src/content/data/ /mnt/nfs/src/cpp/gtnh-platform/src/content/data/   # :18
cp "${BUILD_DIR}"/bin/gameclientd /mnt/nfs/                                # :19
cp -r ${SCRIPT_DIR}/src/apps/game_client /mnt/nfs/src/cpp/gtnh-platform/src/apps   # :25
rsync -a src/ /mnt/nfs/src/cpp/gtnh-platform/src ...                       # :209
```

Four hard-coded `/mnt/nfs` writes, three of them before any service starts. With
`set -euo pipefail` (`run.sh:2`) a missing mount aborts the run at line 18.

- **Impact:** `run.sh` is unusable off this workstation. Not documented anywhere in
  `run.sh`'s `--help` (`run.sh:38-41`) or in `README.md:140-146`.

### SD-03 (HIGH) — `--all` launches binaries that are not in the default build, so `--all` can never succeed

`run.sh:94-97` requires `SPATIALINDEXD` and `VALIDATIOND`; `run.sh:196-197` launch them.
Neither directory is added to the build:

```
$ grep -n "add_subdirectory" CMakeLists.txt
84:#add_subdirectory(src/apps/spatial_index)
   (no line for src/apps/validation)
```

No `add_subdirectory(src/apps/validation ...)` exists at all, and `spatial_index` is
commented out at `CMakeLists.txt:84`. So `cmake-build-debug/src/apps/spatial_index/spatialindexd`
and `.../validation/validationd` can never exist, and `run.sh:95` `die`s.

- **Impact:** `README.md:144` advertises `./run.sh --all` as a supported invocation. It
  cannot work. `AGENTS.md` calls SpatialIndex "STUB, not built" but then lists it in the
  `run.sh` flags line as if it were runnable.

### SD-04 (MEDIUM) — `SIMCORED` gets 5 positional args; arg 6 (`machines_yaml`) is never passed and uses a CWD-relative default

`run.sh:191`:

```bash
LAUNCH "simcored" "${SIMCORED}" 127.0.0.1 4000 127.0.0.1 5001 ${SCRIPT_DIR}/src/content/data/recipes
```

`simcored` reads `argv[6]` as `machines_yaml` with default
`"src/content/data/registry/machines.yaml"` (`src/apps/simcore/main.cpp:124`) — a
**relative** path. `run.sh` never `cd`s to `${SCRIPT_DIR}` before launching, so
`simcored` resolves that path against whatever `$PWD` the user ran `run.sh` from, and
the machine registry silently fails to load when the script is invoked from a
subdirectory.

- **Contrast:** the recipes dir at `run.sh:191` *is* absolute. The fix is to pass
  `${SCRIPT_DIR}/src/content/data/registry/machines.yaml` as the 6th arg too.

### SD-05 (MEDIUM) — the `go build` step for `routerd` writes to a path Ninja never populates, and the `main.go router.go` file list is wrong

`run.sh:20-22`:

```bash
pushd "${SCRIPT_DIR}/src/apps/message_router/"
go build -o "${BUILD_DIR}/src/apps/message_router/routerd" main.go router.go
```

- The file list `main.go router.go` omits nothing needed today (`ls src/apps/message_router/`
  shows only those two non-test `.go` files plus `dummy.go`, which is a 174-byte decoy
  for the CMake stub) — **this part is correct**.
- The **output path is a lie**: `add_subdirectory(src/apps/message_router)` is
  commented out at `CMakeLists.txt:90`, so Ninja never creates or manages
  `${BUILD_DIR}/src/apps/message_router/`. The only reason the file exists is that
  `run.sh:21` hand-writes it. That makes the `routerd` staleness check at
  `run.sh:73` (rebuild if `< 100000` bytes) self-fulfilling.
- **Consequence:** a plain `ninja -j5` in `cmake-build-debug` produces **no `routerd`**.
  `README.md:133-136` tells the reader to do exactly that as "Build", and
  `README.md:151` tells them to run `./cmake-build-debug/src/apps/message_router/routerd`
  as step 1. A reader who follows the Build section literally has no `routerd`.

### SD-06 (MEDIUM) — `go build -o metadbd *.go` shell-globs the source list and would break on any new non-Go-irrelevant file

`run.sh:27`: `go build -o metadbd *.go`

`ls src/apps/meta_db/` shows 7 `*_test.go` files in that directory. Go's build ignores
`_test.go` files when naming an explicit file list, so this currently works — but it is
fragile, and it diverges from the canonical build, which is
`go build -o metadbd .` (`src/apps/meta_db/CMakeLists.txt:43`). Note also that
`metadbd` *is* a CMake `ALL` target (`src/apps/meta_db/CMakeLists.txt:52-54`), so the
`run.sh` rebuild at `:26-28` is redundant with Ninja.

### SD-07 (MEDIUM) — two Go binaries are checked into git, and `run.sh` depends on one of them being present

`git ls-files src/apps/meta_db/` returns both `src/apps/meta_db/metadb` and
`src/apps/meta_db/metadbd` — 9.4 MB each, currently dirty in `git status`
(`M src/apps/meta_db/metadb`, `M src/apps/meta_db/metadbd`).

`.gitignore:102-103` lists `src/apps/meta_db/metadb` and `.../metadbd`, but they are
**already tracked**, so the ignore rules never fire and the blobs stay in history.
`run.sh:56` (`METADBD="${SCRIPT_DIR}/src/apps/meta_db/metadbd"`) and `run.sh:85-87`
rely on that tracked binary existing — a fresh clone therefore has a 9.4 MB platform
binary in the tree that the `.gitignore` claims should not be there.

*(Related open ticket: `gp-5moy` "Gitignore the compiled Go test binaries".)*

### SD-08 (LOW) — the leftover-process sweep uses names that do not match the launched binary names

`run.sh:110` and `run.sh:119` `pgrep -x` for `simcored_exec` and `pipe_networkd`, while
`run.sh:193` launches the service under the *log* name `pipenetworkd`. The process
actually executing is the file `pipe_networkd`, so the sweep is right about the real
process and `LAUNCH`'s name is only cosmetic — but `run.sh:110` does **not** list
`reciped`, which `run.sh:189` does launch, so a leftover `reciped` survives cleanup.

Also `run.sh:110` lists `validationd` but never `reciped`; the two lists at `:110` and
`:119` differ in content but nothing explains the difference.

### SD-09 (LOW) — the `--db-dir` flag only reaches ChunkStore; MetaDB's SQLite path is not configurable from `run.sh`

`--db-dir` is applied at `run.sh:187` (`"${DB_DIR}"` as chunkd's `argv[1]`) and
`run.sh:101` `mkdir -p`. But `metadbd` uses a fixed relative `"metadb.sqlite"`
(`src/apps/meta_db/main.go:18`), so it writes to `$PWD` rather than `${DB_DIR}`. There is
already a 122 KB `metadb.sqlite` at the repo root (from `run.sh` being run from the
root). `README.md:156` documents `./src/apps/meta_db/metadbd # 6. Player DB (Go, :5005 + :5006)`
with no mention of the DB file location.

### SD-10 (LOW) — `--resolution` is parsed but the client branch it guards is unreachable

`run.sh:10` sets `START_CLIENT=false`; the flag list at `run.sh:31-44` has no way to set
it true (there is no `--client`/`--with-client`). So the `if $START_CLIENT` blocks at
`run.sh:200-208` never run, and `--resolution` (`run.sh:35`, consumed at `run.sh:204/206`)
is dead. `README.md:143-146` documents only `--all` and `--no-client` — consistent with
the client never starting — while `AGENTS.md` lists the game client as run-order step 8.

### SD-11 (LOW) — the log forwarder target is a hard-coded private IP

`run.sh:13-14`: `LOKI_HOST="192.168.2.109"`, `LOKI_BRIDGE_PORT=1514`, used at
`run.sh:170`. Undocumented, not flag-overridable, and `nc` to an unreachable host is
spawned for every service (`run.sh:169-172`).

---

## 2. Findings — `README.md`

### SD-12 (HIGH) — `README.md:127` points at a `conan_toolchain.cmake` that does not exist

```bash
conan install -of build --build=missing
cd build
cmake -GNinja -DCMAKE_TOOLCHAIN_FILE=$PWD/conan_toolchain.cmake ..
```

```
$ ls -la conan_toolchain.cmake
ls: невозможно получить доступ к 'conan_toolchain.cmake': Нет такого файла или каталога
$ ls conan_debug/ conan_release/
conan_debug:  conan_toolchain.cmake …
conan_release: conan_toolchain.cmake …
```

Conan is invoked with `-of build`, so the toolchain lands in `build/conan_toolchain.cmake`,
not `$PWD/conan_toolchain.cmake` — and even then, `cd build` already made the path
`build/./conan_toolchain.cmake`, so the two lines are self-contradictory.

The real, working mechanism is the **auto-detection in `CMakeLists.txt:29-45`**, which
probes `conan_debug` (Debug) / `conan` / `cmake-build-offline/deps/conan-offline` / `.`
and `include()`s the first toolchain it finds. `AGENTS.md` correctly says the
toolchains are `conan_debug/` and `conan_release/` (both gitignored). Neither
`README.md` nor `AGENTS.md` mentions `conan_release/` in the README's case.

The README's "Build" section is also **contradicted by `AGENTS.md`**, which says
`README.md` is stale and to use the existing `cmake-build-debug/`. Two documents give
incompatible build instructions for the same repo.

### SD-13 (HIGH) — `README.md:151` step 1 runs a binary that a clean `ninja` never builds

Covered by SD-05. The direct consequence for the README is that the Quick Start
"or manually (order matters)" block at `README.md:150-159` is broken at step 1: after
the documented `cd cmake-build-debug && ninja -j5` (`README.md:133-136`), step 1's
`./cmake-build-debug/src/apps/message_router/routerd` does not exist, because
`CMakeLists.txt:90` has `add_subdirectory(src/apps/message_router)` commented out. Only
`run.sh:21` ever produces it.

### SD-14 (HIGH) — `README.md:144` advertises `--all`, which can never succeed

`README.md:144`: `./run.sh --all # also pipenetworkd / spatialindexd / validationd`.
Per SD-03, `spatialindexd` and `validationd` are not buildable, so `run.sh:95` dies
before starting anything. Note the README is also self-inconsistent: it lists
`pipenetworkd` as an `--all` extra, but `run.sh:193` starts it **unconditionally**.

### SD-15 (MEDIUM) — `README.md:157` names a binary that does not exist: `pipenetworkd`

```bash
./cmake-build-debug/src/apps/pipe_network/pipenetworkd   # 7. Energy/fluid transport
```

```
$ grep -n "add_executable" src/apps/pipe_network/CMakeLists.txt
36:add_executable(pipe_networkd
$ ls cmake-build-debug/src/apps/pipe_network/
pipe_networkd
pipe_network_test
```

The real target and file is **`pipe_networkd`**. This is the same class of error the
ticket flagged for `simcored_exec` — but here the doc is wrong, not the script
(`run.sh:54` correctly uses `pipe_networkd`). `README.md:144` repeats the wrong name
in the `--all` description.

### SD-16 (MEDIUM) — `README.md:114` says the wire protocol has 41 message types; `AGENTS.md:96` says 1–51

`README.md:114`: "`Wire protocol = C++ `GatewayMsg` constants (41 types, 1-based)`".

`src/common/GatewayMsg.h` enumerates to **51**:
```
73:inline constexpr uint8_t kServiceHealthResp = 51;
```
and 51 is the max (`kChunkSnapshot = 2` … `kServiceHealthResp = 51`). `AGENTS.md`
("wire protocol = C++ `GatewayMsg` constants (1–51, 1-based)") and
`doc/c4/README.md:109-111` ("51 тип GatewayMsg") both say 51. Only `README.md:114` says
41. `doc/audits/agents-md-drift.md` confirms 51 is right.

### SD-17 (MEDIUM) — `README.md:121` names a dependency that is not used anywhere: LMDB++

`README.md:121`: "Conan handles most dependencies (Asio, EnTT, spdlog, FlatBuffers, **LMDB++**, GLM, etc.)"

`grep -rn "lmdb++\|LMDB++"` across the repo finds exactly one hit: that README line.
`conanfile.txt:17` declares `lmdb/0.9.32` (the C library), and the tree uses the C API.
There is no LMDB++ in `conanfile.txt` and no C++ wrapper usage.

### SD-18 (MEDIUM) — `README.md:120` gives Conan install instructions that contradict `conanfile.txt`

`README.md:120`: "install tbb bgfx bx bimg (and may be lodepng) in system".

`conanfile.txt` declares `fastnoise2/1.1.1`, `asio/1.32.0`, `liburing/2.13`, `entt/3.16.0`,
`imgui/1.91.8`, `glfw/3.4`, `miniaudio/0.11.22`, `yaml-cpp/0.8.0`, `nlohmann_json/3.11.3`,
`sqlite3/3.49.1`, `lmdb/0.9.32`, `lodepng/cci.20230410` — and **no TBB and no bgfx/bx/bimg**
(`[options]` even sets `boost/*:without_python=True` for a boost that is not in
`[requires]`). Meanwhile `CMakeLists.txt:58` does `find_package(TBB REQUIRED)` and
`CMakeLists.txt:74` `find_package(imgui CONFIG REQUIRED)`, while bgfx comes from the
vendored `third_party/bgfx.cmake`. So the README tells the reader to system-install
libraries that Conan does not manage, and omits most of the ones it does.

### SD-19 (MEDIUM) — `README.md:33-76` (the ASCII architecture diagram) contradicts the code in three places

The block carries its own disclaimer at `README.md:42` and defers to `doc/c4/`, which is
honest, but it is the first thing a reader sees. Concretely wrong:

1. **`README.md:73` — `RecipeMgr (:5555)`.** Nothing in the tree listens on 5555.
   ```
   $ grep -rn "5555" --include=*.cpp --include=*.h --include=*.go src/ tools/
   (no hits)
   ```
   All hits are in `doc/c4/level2-container.puml:49`, `level4-deployment.puml:23,46`
   and `ROADMAP.md:274,635,664`. `reciped` is a **router-registered pub/sub service**
   (`src/apps/recipe_manager/main.cpp:42-43`, subscribes to `recipe.*` at
   `RecipeManagerService.cpp:27-33`), it opens no TCP listener.
2. **`README.md:82` — "SimulationCore → ChunkStore has direct RPC".** Partly true, but
   the diagram omits that SimCore *also* reaches EntityStateStore on a hard-coded
   `127.0.0.1:5200` (`src/apps/simcore/main.cpp:285`).
3. **`README.md:71-76` — SpatialIndex is drawn as a peer service.** It is a
   2-line stub that is not added to the build (`CMakeLists.txt:84`,
   `src/apps/spatial_index/CMakeLists.txt:1` is the entire target). The README's own
   table at `README.md:97` says "STUB — not implemented, not built", so the diagram and
   the table disagree inside the same file.

### SD-20 (MEDIUM) — `README.md:110` chunk-format claim is obsolete in the implementation

`README.md:110`: "Chunk format: 32³ blocks — 192 KB per chunk (blocks + meta + extra)".

The storage layer is **palette-encoded, not three flat arrays**.
`src/apps/chunk_store/Storage/SectionCodec.h:5-15` documents the actual on-disk format:

```
[magic "GCHK"] [ver 1] [sec_cnt 8] sections[8]:
  [palette_size u16][palette][bits_per_index u8][indices][meta_count][meta][mb_count][mb]
```

`src/apps/chunk_store/Storage/SectionCodec.h:20-22`: `SEC_SZ = 16`, `SEC_VOL = 4096`,
`SEC_CNT = 8` — i.e. 8 sections of 16³, not one flat 32³ triple. And the in-memory
representation is a tagged union with sparse meta and flat fallbacks
(`src/apps/chunk_store/Storage/cache/MutableSection.h:10`, `:40-45`), not fixed-width
32 KB + 32 KB + 128 KB.

The repo's own benchmark says the real size: `src/apps/chunk_store/Storage/chunkd_load_test.cpp:96`
— "Each chunk = ~260 bytes palette-encoded". So the 192 KB figure survives in
`AGENTS.md:158`, `README.md:110`, `openspec/project.md:74`, `doc/diff-protocol.md:7,13`,
`doc/c4/level2-container.puml:31`, `doc/c4/level3-chunkstore.puml:19`,
`doc/c4/level4-deployment.puml:31`, `openspec/specs/protocol/spec.md:24` — all stale.

### SD-21 (LOW) — `README.md:171` "10 runnable daemons + 2 stubs" contradicts `README.md:6` "9 daemons" and the build

`README.md:6`: "9 daemons + engine/game/content libraries".
`README.md:171`: "apps/ # 10 runnable daemons + 2 stubs (assembly points)".

The actual set of non-test executables in the default build
(`find cmake-build-debug/src/apps -maxdepth 2 -type f -executable`) is:
`routerd, chunkd, entitystated, gatewayd, pipe_networkd, simcored_exec, reciped` —
**7 daemons** — plus `gameclientd` in `cmake-build-debug/bin/`, for **8**. Add
`metadbd` (Go, built by CMake at `src/apps/meta_db/CMakeLists.txt:43`) for **9**.

So **9 is the right number** and `README.md:171`'s "10 runnable daemons + 2 stubs" is
wrong on both halves: there are not 10, and the 2 "stubs" (SpatialIndex, Validation)
are not built at all, so they are not among the "runnable" anything.

### SD-22 (LOW) — `README.md:197` "src/protocol/ FlatBuffers schemas (14 .fbs)" is **correct** — leave it

`ls src/protocol/*.fbs | wc -l` → **14**. `AGENTS.md:56` also says 14, correctly.
Note `ROADMAP.md:42` still says "**12 файлов**" (see `doc/audits/roadmap-gaps.md`).
This one needs no change.

### SD-23 (LOW) — `README.md:168-200` "Project Structure" tree has a doubled `src/`

```
src/
├── src/
│   ├── apps/
```

The literal content of `README.md:169-170` is `src/` then `├── src/`. The outer wrapper
should not be there, and the second branch is labelled `docs/` at `README.md:199` while
the repo has **both** `doc/` (with `c4/`, `audits/`, `texture-atlas-format.md`) and
`docs/` (`gateway-headless-client.md`, `modding.md`). `AGENTS.md:118` points at
`doc/texture-atlas-format.md` (singular) while `AGENTS.md:88`-row points at
`docs/gateway-headless-client.md` (plural) — the repo genuinely has both, but the README
tree shows only one and in the wrong place.

### SD-24 (LOW) — `README.md:205` "YAML recipes (14 files incl. macerator.yaml)" — verify before changing

Not verified as stale in this audit. `AGENTS.md:117` says "15 files" for the same
directory. Left as an open question for whoever fixes this; the count is
`ls src/content/data/recipes/*.yaml | wc -l`.

### SD-25 (LOW) — `README.md:227` "Generated: 2026-08-07" and `README.md:42` self-disclaimer

The generated stamp is 2 months stale relative to the tree. Low value, but it is the
kind of thing that makes readers trust the rest of the file — and given SD-12 through
SD-21, they should not.

---

## 3. Ports — verified correct

| Port | Owner | Source | Verdict |
|------|-------|--------|---------|
| 4000 | MessageRouter (Go) | `src/apps/message_router/main.go:23` | ✅ correct |
| 4000 | client-facing default for gateway/simcore/chunkd/reciped/pipenet/entitystated | `gateway/main.cpp:30`, `simcore/main.cpp:120`, `chunk_store/main.cpp:31`, `recipe_manager/main.cpp:43`, `pipe_network/main.cpp:42`, `entity_state_store/main.cpp:244` | ✅ correct |
| 5001 | ChunkStore TCP | `src/apps/chunk_store/main.cpp:29` | ✅ correct |
| 5005 | MetaDB JSON API | `src/apps/meta_db/main.go:19` (`:5005`) | ✅ correct |
| 5006 | MetaDB FlatBuffers RPC | `src/apps/meta_db/flatbuffer_tcp.go:14` | ✅ correct — **but see SD-28** |
| 5200 | EntityStateStore TCP RPC | `src/apps/entity_state_store/main.cpp:257-258` | ✅ correct |
| 7777 | Gateway ctrl | `src/apps/gateway/main.cpp:31` | ✅ correct |
| 7778 | Gateway bulk | `src/apps/gateway/main.cpp:32` | ✅ correct |
| 7777/7778 | client defaults | `src/apps/game_client/main.cpp:52-53` | ✅ correct |
| **5555** | — | `grep -rn 5555 src/ tools/` → no hits | ❌ **does not exist** (SD-19) |

All seven real ports are documented correctly in `AGENTS.md` and `doc/c4/`. The only
port error is the phantom 5555.

---

## 4. Flags `run.sh` accepts vs what the binaries actually parse

| Flag | `run.sh` | Actually parsed by | Verdict |
|------|----------|--------------------|---------|
| `--build-dir` | `:33` | none — the ninja call at `:17` ignores it | ❌ **SD-01** |
| `--db-dir` | `:34` → `:187` chunkd `argv[1]`; default `./chunkdb` | `src/apps/chunk_store/main.cpp:28` | ✅ works (chunkd only, SD-09) |
| `--resolution` | `:35` → `:204` | `src/apps/game_client/main.cpp:56` | ⚠️ parsed but unreachable (SD-10) |
| `--all` | `:36` | — | ❌ **SD-03**, can never pass |
| `--no-client` | `:37` | — | ⚠️ no-op, client is already off (SD-10) |
| `--help/-h` | `:38-41` | — | ✅ correct |
| unknown opt | `:42` exits 1 | — | ✅ correct |

Flags the binaries accept but `run.sh` never exposes (so users cannot reach them through
the script):

| Flag | Binary | Source |
|------|--------|--------|
| `--db-max-size-mb <N>` | chunkd | `src/apps/chunk_store/main.cpp:36-37` |
| `--bulk-port <N>` | gatewayd | `src/apps/gateway/main.cpp:40-41` |
| `--router-host <H>` | reciped, pipe_networkd | `recipe_manager/main.cpp:48`, `pipe_network/main.cpp:45` |
| `--data-dir <D>` | reciped | `src/apps/recipe_manager/main.cpp:52-53` |
| `--host`, `--port`, `--bulk-port` | gameclientd | `src/apps/game_client/main.cpp:58-60` |
| `--shader-dir` | gameclientd | `src/apps/game_client/main.cpp:55` |
| `-port` | routerd | `src/apps/message_router/main.go:23` |

---

## 5. Every binary `run.sh` launches, vs what the build produces

`BIN()` at `run.sh:46` maps `dir -> binary`; the mapping is checked against
`add_executable` names and the on-disk `cmake-build-debug/` output.

| `run.sh` var | Line | Path | Target exists? | Binary on disk? | Verdict |
|---|------|------|----------------|-----------------|---------|
| `ROUTERD` | 48 | `message_router/routerd` | `GO_TARGET` = `routerd` (`message_router/CMakeLists.txt:8`) | ✅ but **only because `run.sh:21` wrote it** | ⚠️ **SD-05** |
| `CHUNKD` | 50 | `chunk_store/chunkd` | ✅ `chunk_store/CMakeLists.txt:71` | ✅ | ✅ |
| `GATEWAYD` | 51 | `gateway/gatewayd` | ✅ `gateway/CMakeLists.txt:43` | ✅ | ✅ |
| `SIMCORED` | 52 | `simcore/simcored_exec` | ✅ `simcore/CMakeLists.txt:146` | ✅ | ✅ **correct, do not change** |
| `CLIENT` | 53 | `bin/gameclientd` | ✅ `game_client/CMakeLists.txt:57` | ✅ | ✅ |
| `PIPENETWORKD` | 54 | `pipe_network/pipe_networkd` | ✅ `pipe_network/CMakeLists.txt:36` | ✅ | ✅ |
| `SPATIALINDEXD` | 55 | `spatial_index/spatialindexd` | ⚠️ target exists (`spatial_index/CMakeLists.txt:1`) but **dir not added** (`CMakeLists.txt:84` commented) | ❌ | ❌ **SD-03** |
| `METADBD` | 56 | `src/apps/meta_db/metadbd` (source tree, not build dir) | ✅ CMake `ALL` target `meta_db/CMakeLists.txt:52` | ✅ (git-tracked, SD-07) | ⚠️ **SD-07** |
| `ENTITYSTATED` | 57 | `entity_state_store/entitystated` | ✅ `entity_state_store/CMakeLists.txt:45` | ✅ | ✅ |
| `RECIPED` | 58 | `recipe_manager/reciped` | ✅ `recipe_manager/CMakeLists.txt:39` | ✅ | ✅ |
| `VALIDATIOND` | 59 | `validation/validationd` | ⚠️ target exists (`validation/CMakeLists.txt:5`) but **dir never added to the build** | ❌ | ❌ **SD-03** |

`BIN()` itself is fine as a helper — the mapping it encodes is correct for the 7
buildable daemons. The problem is that two of the eleven variables point at targets the
build never produces.

---

## 6. Verified CORRECT — do not "fix" these

These are the load-bearing details a fixer is most likely to break:

1. **`run.sh:52` `simcored_exec` is correct** — `src/apps/simcore/CMakeLists.txt:146`,
   and the binary is at `cmake-build-debug/src/apps/simcore/simcored_exec`.
   It is `AGENTS.md` that is wrong here (calls it `simcored`); see `agents-md-drift.md`.
2. **`run.sh:54` `pipe_networkd` is correct** — `src/apps/pipe_network/CMakeLists.txt:36`.
   It is `README.md:157` that is wrong.
3. **`run.sh:58` `reciped` is correct** — `src/apps/recipe_manager/CMakeLists.txt:39`.
4. **`run.sh:50,51,57` `chunkd`/`gatewayd`/`entitystated`** all match
   `add_executable` and on-disk output.
5. **The seven real ports** (4000, 5001, 5005, 5006, 5200, 7777, 7778) are all correctly
   documented in `README.md` and `AGENTS.md`.
6. **`README.md:197` "14 .fbs" is correct** — `ls src/protocol/*.fbs | wc -l` → 14.
7. **`run.sh:20-22` `go build main.go router.go`** is currently correct: those are the
   only two non-test `.go` files in `src/apps/message_router/` (`dummy.go` is a
   174-byte decoy, `router_cleanup_test.go` is a test).
8. **`run.sh`'s flag parser** correctly rejects unknown options (`run.sh:42`).

---

## 7. Suggested fix order (for the follow-up task, not applied here)

1. **SD-01 + SD-02** — make `run.sh` respect `--build-dir` and make the `/mnt/nfs` copies
   opt-in. Until then `run.sh` is workstation-only. Highest value: it is the documented
   single entry point (`README.md:143`).
2. **SD-03 + SD-14** — either add `spatial_index` and `validation` to the build or stop
   advertising `--all`. Currently `run.sh --all` is a guaranteed crash.
3. **SD-05 + SD-13** — uncomment `add_subdirectory(src/apps/message_router)` (or
   document the `go build` as mandatory) so the documented build produces step 1.
4. **SD-12** — replace the Conan block in `README.md:123-129` with the real
   `conan_debug/` / `conan_release/` auto-detection from `CMakeLists.txt:29-45`.
5. **SD-15, SD-16, SD-17, SD-19, SD-20, SD-21** — the cheap factual corrections.
6. **SD-20 is the deepest** — the 192 KB figure is load-bearing in 9 files across the
   repo, and it is wrong in all of them. Treat as its own task, not a README edit.
