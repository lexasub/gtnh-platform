<!-- OPENSPEC:START -->
# OpenSpec Instructions
if stuck [optional] see c4 diagram doc/c4/README.md
These instructions are for AI assistants working in this project.

Always open `@/openspec/AGENTS.md` when the request:
- Mentions planning or proposals (words like proposal, spec, change, plan)
- Introduces new capabilities, breaking changes, architecture shifts, or big performance/security work
- Sounds ambiguous and you need the authoritative spec before coding

Use `@/openspec/AGENTS.md` to learn:
- How to create and apply change proposals
- Spec format and conventions
- Project structure and guidelines

Keep this managed block so 'openspec update' can refresh the instructions.

<!-- OPENSPEC:END -->

# GTNH Platform Knowledge Base

**Generated**: 2026-08-07

## OVERVIEW

Distributed Minecraft-style platform with C++ performance core + Go sidecars. Binary protocol (FlatBuffers + TCP) connects 13 service directories (10 real services + spatial_index stub + storage_interfaces + validation) via MessageRouter.
Linux-only project. No Windows/macOS support.

## STRUCTURE

```
src/
├── src/
│   ├── services/
│   │   ├── message_router/    # Go pub/sub broker, service discovery
│   │   ├── gateway/           # TCP gateway, io_uring, interest mgmt
│   │   ├── chunk_store/       # LMDB-backed block storage, io_uring
│   │   ├── world_generator/   # Terrain + ore/tree gen (library, no binary)
│   │   ├── simulation_core/   # ECS, multiblocks L2/L3, quests, 20 Hz tick
│   │   ├── pipe_network/      # Energy/fluid/item flow graphs
│   │   ├── spatial_index/     # STUB — not built (R-tree/Octree planned)
│   │   ├── entity_state_store/ # Entity state persistence, TCP RPC
│   │   ├── meta_db/           # Player saves, quests, inventories (Go)
│   │   ├── recipe_manager/    # Standalone recipe RPC service (:5555)
│   │   ├── storage_interfaces/ # Header-only storage interfaces
│   │   ├── validation/        # Item/block validation (not in default build)
│   │   └── game_client/       # bgfx render, ImGui, input, physics
│   └── protocol/              # FlatBuffers schemas (12 .fbs)
├── src/libs/                  # libgtnh-net, quest_lib, recipe_manager_lib, ...
├── cmake-build-debug/         # CMake build directory (Conan toolchain)
├── data/                      # YAML recipes, item registry
└── docs/                      # Service documentation
```

## SERVICES

| Service | Subdir | Language |
|---------|--------|----------|
| MessageRouter | `src/services/message_router/` | Go |
| Gateway | `src/services/gateway/` | C++ |
| ChunkStore | `src/services/chunk_store/` | C++ |
| WorldGenerator | `src/services/world_generator/` | C++ (library) |
| SimulationCore | `src/services/simulation_core/` | C++ |
| PipeNetwork | `src/services/pipe_network/` | C++ |
| SpatialIndex | `src/services/spatial_index/` | C++ (STUB, not built) |
| EntityStateStore | `src/services/entity_state_store/` | C++ |
| MetaDB | `src/services/meta_db/` | Go |
| RecipeManager | `src/services/recipe_manager/` | C++ (RPC service, :5555) |
| StorageInterfaces | `src/services/storage_interfaces/` | C++ (headers only) |
| Validation | `src/services/validation/` | C++ (not in default build) |
| GameClient | `src/services/game_client/` | C++ |

Key libs: RecipeManagerLib `src/libs/recipe_manager_lib/`, libgtnh-net `src/libs/libgtnh-net/` (io_uring networking), quest_lib `src/libs/quest_lib/`, machine_registry `src/libs/machine_registry/`.

## WHERE TO LOOK

| Task                      | Location                      | Notes                              |
|---------------------------|-------------------------------|------------------------------------|
| Binary protocol schema    | `src/protocol/`                       | 12 FlatBuffers `.fbs` files; wire protocol = C++ `GatewayMsg` constants (41, 1-based) — `GatewayPayload` union in gateway.fbs is stale |
| Internal message routing  | `src/services/message_router/`       | Go channels, pub/sub topics        |
| Client connections        | `src/services/gateway/`              | TCP accept, interest management    |
| Block data storage        | `src/services/chunk_store/`          | LMDB, chunk serialization          |
| Terrain generation        | `src/services/world_generator/`      | Noise functions, biomes            |
| ECS, multiblocks, mobs    | `src/services/simulation_core/`      | EnTT, pattern matching             |
| Energy/liquid networks    | `src/services/pipe_network/`         | Graph algorithms, flow solving     |
| Spatial queries           | `src/services/spatial_index/`        | STUB — not implemented, not built   |
| Entity state persistence  | `src/services/entity_state_store/`   | LMDB-backed, TCP RPC port 5200     |
| Player saves              | `src/services/meta_db/`              | SQLite, transactional saves        |
| Quest system              | `src/services/meta_db/` + `src/services/game_client/` | quest_lib data model, quest.fbs protocol, QuestBookWindow |
| Quest system              | `src/services/meta_db/` + `src/services/game_client/` | quest_lib data model, quest.fbs protocol, QuestBookWindow |
| Crafting recipes          | `data/recipes/`                      | YAML files per machine type (14 files) |
| Item registry             | `data/registry/`                     | items.csv, items.db, machines.yaml, ores.json |
| Recipe system             | `src/libs/recipe_manager_lib/` + `src/services/recipe_manager/` | YAML recipes, ConditionEvaluator (MachineState from ECS) |
| Rendering, input, audio   | `src/services/game_client/`          | bgfx, GLFW, ImGui                  |

## CONVENTIONS

- **FlatBuffers**: Single schema across all services (`namespace Protocol`)
- **Event-driven**: `BlockChanged` published by ChunkStore → caught by SimulationCore
- **Language boundaries**: Hot path = C++ only. Sidecars = Go/Python via `IExternalLogic`
- **Zero-copy**: Chunk data flows FlatBuffer → LMDB mmap → TCP send buffer

## ANTI-PATTERNS

- ❌ Breaking multiblock across chunk boundaries without `SetBlockMeta`
- ❌ Using Go for ChunkStore/SimulationCore (GC pauses unacceptable)
- ❌ Parsing JSON in Gateway (must be zero-copy binary only)
- ❌ Storing multiblock controllers in ChunkStore (Simulation owns them)

## BUILD & RUN

**NEVER rebuild from scratch.** Dependencies are pre-installed. See README.md Build section for full details.

**NEVER delete `cmake-build-debug/` or `cmake-build-release/`** — they contain Conan-generated toolchain files. Recreating them requires `conan install` + network access.

```bash
# Build (use existing cmake-build dir — has Conan toolchain already)
cd cmake-build-debug
ninja -j5

# Or for release build:
cd cmake-build-release
ninja -j5

# Run (order matters, from repo root)
./cmake-build-release/routerd            # 1. Internal pub/sub (Go, :4000)
./cmake-build-release/chunkd             # 2. World persistence (C++, :5001)
./cmake-build-release/entitystated       # 3. Entity state (C++, :5200)
./cmake-build-release/gatewayd           # 4. TCP gateway (C++, :7777 ctrl + :7778 bulk)
./cmake-build-debug/src/services/simulation_core/simcored_exec  # 5. Simulation (C++, 20Hz tick)
./src/services/meta_db/metadbd           # 6. Player DB (Go, :5005 + :5006)
./cmake-build-debug/src/services/pipe_network/pipenetworkd  # 7. Energy/fluid transport (C++)
./cmake-build-debug/bin/gameclientd      # 8. Game client (C++, bgfx)
```

**Alternative**: `./run.sh` — builds ninja in cmake-build-debug, rebuilds Go services, starts everything in order (with `--all` also pipenetworkd/spatialindexd/validationd; `--no-client` to skip the client).

**If build fails**: Check `conan install` was run. See README.md for Conan setup.

Always compile and verify changes incrementally after each small logical chunk, never batched, to avoid hitting the $1/day API budget during final build checks.

**Tests**:
```bash
cd cmake-build-debug && ctest --output-on-failure -j$(nproc)
```

## NOTES

- Chunk format: 32 KB + 32 KB + 128 KB = 192 KB per chunk
- Multiblock ID stored in meta-layer (O(1) lookup without scanning world)
- MessageRouter uses Go channels — 100k concurrent pub/sub topics are cheap

## LIBRARY DECISIONS

### C++ Stack
| Library | Purpose | Why |
|---------|---------|-----|
| **Asio** | TCP server, async IO, io_uring backend | Zero-copy recv→FlatBuffer, coroutine-friendly (C++20), standard, production-grade |
| **FlatBuffers C++** | Binary protocol | Single schema, `GetRoot<Message>()` zero-copy parsing, no allocations |
| **EnTT** | ECS (Entity Component System) | Fastest C++ ECS, sparse sets, O(1) iteration, cache-miss friendly |
| **LMDB / LMDB++** | Chunk persistence | Read-optimized, mmap, zero-copy reads, ACID, embedded (no separate process) |
| **FastNoiseLite** | Terrain generation | Header-only, SIMD-friendly, 3D Perlin/Simplex/cellular, fractal Brownian noise |
| **GLM** | Math (vec3, matrices, noise coords) | Header-only, consistent syntax across services |
| **spdlog** | Logging | Header-only, async mode, production-grade formatting |
| **Boost.Geometry (R-tree)** | Spatial index | `bgi::rtree<AABB>` for multiblock/entitiy queries, O(log n) bounding-box search |
| **bgfx** | Cross-API render | Unified shaders, GL/Vulkan/D3D/Metal, one codebase |
| **GLFW** | Windowing + input | Simple, stable, no context management conflicts |
| **miniaudio** | Audio (footsteps, blocks, UI) | Header-only, lightweight |

### Go Stack
| Library | Purpose | Why |
|---------|---------|-----|
| **stdlib `net`** | TCP server/client | Production-ready, goroutines per connection idiomatic, no external deps |
| **stdlib `database/sql`** | SQL abstraction | Clean, idiomatic |
| **mattn/go-sqlite3** | SQLite driver (CGO) | Fastest Go SQLite, production-grade |
| **FlatBuffers Go** | Binary protocol | Single schema, zero-allocation parsing |

### What's NOT used (and why)
- **gRPC** — overhead for internal pub/sub; Go channels + FlatBuffers = lighter
- **ZeroMQ** — C dependency, breaks Go purity; stdlib + channels = native
- **SQLite vs LMDB** — SQLite = write-optimized, WAL log; LMDB = read-optimized, mmap, no separate WAL
- **RocksDB** — write-optimized, unnecessary overhead for chunk reads
- **Lua/Python mod runtime** — deferred. Mods via C++ `.so/.dll` loaded with `dlopen`. Scripting later.
- **AssetServer** — deferred. Assets embedded in Client or proxied via Gateway TCP. QUIC/HTTP/3 when scale demands it.
- **JSON parsing in Gateway** — forbidden. Must be zero-copy binary only.

## SERVICE BOUNDARIES (critical)

### ChunkStore vs SimulationCore
**ChunkStore** = dumb storage. Only knows `block_id + meta + mb_id`. Never understands "electrolyser" or "pipe".

**SimulationCore** = owns Multiblock Controllers. Holds `entt::registry` with `MultiblockController{mb_id, anchor, blocks[...]}`.

When multiblock forms:
```
Client → Gateway → SimulationCore
                          ↓ (RPC: GetBlock in pattern radius)
                      ChunkStore
                          ↓ (match found)
                  Create MultiblockController in ECS
                          ↓ (RPC: SetBlockMeta for ALL pattern blocks)
                      ChunkStore (writes mb_id into chunk meta-layer)
```

On chunk unload:
1. ChunkStore marks chunk `pending_unload`
2. Asks SimulationCore: "here is list of mb_id in this chunk, can I unload?"
3. Simulation checks anchor:
   - **anchor INSIDE chunk** → serializes MB to MetaDB, returns `release`
   - **anchor OUTSIDE chunk** → returns `hold` (MB active, keep chunk in memory)
4. ChunkStore unloads **only on `release`**

### PipeNetwork separate from SimulationCore
Simulation reports: "network #3: 5 pipes, 2 inputs, 3 outputs".
PipeNetwork solves graph per tick, returns `flow_map`.
If network unchanged 5 seconds → skip tick (cache).
Can run 2 instances (one per dimension) without interference.

### EntityStateStore vs MetaDB
**EntityStateStore** (C++): Persistent state for world-bound entities (tile entities, machine state). LMDB-backed. Topics: entity.state.get/set, TCP RPC port 5200.

**MetaDB** (Go): Player-bound data (inventories, position, stats). SQLite. Connected to MessageRouter via router_client.go.

## TODO

- [ ] Pause menu / settings window in game client (missing)
- [ ] Sound: miniaudio linked in CMake, no audio code yet
- [ ] SpatialIndex: implement R-tree/Octree (currently 2-line stub, not built)
- [ ] Dedicated Drill UI window (only tooltip so far)
- [ ] Resolve GatewayMsg C++ constants vs FlatBuffers `GatewayPayload` union divergence
- [ ] Server-authoritative grid state via TileEntityStore RPC

---

**Generated**: 2026-08-10 | **Branch**: main

<!-- BEGIN BEADS INTEGRATION v:1 profile:minimal hash:7510c1e2 -->
## Beads Issue Tracker

This project uses **bd (beads)** for issue tracking. Run `bd prime` to see full workflow context and commands.

### Quick Reference

```bash
bd ready              # Find available work
bd show <id>          # View issue details
bd update <id> --claim  # Claim work
bd close <id>         # Complete work
```

### Rules

- Use `bd` for ALL task tracking — do NOT use TodoWrite, TaskCreate, or markdown TODO lists
- Run `bd prime` for detailed command reference and session close protocol
- Use `bd remember` for persistent knowledge — do NOT use MEMORY.md files

**Architecture in one line:** issues live in a local Dolt DB; sync uses `refs/dolt/data` on your git remote; `.beads/issues.jsonl` is a passive export. See https://github.com/gastownhall/beads/blob/main/docs/SYNC_CONCEPTS.md for details and anti-patterns.

## Session Completion

1. **File issues for remaining work** - Create issues for anything that needs follow-up
2. **Run quality gates** (if code changed) - Tests, linters, builds
3. **Update issue status** - Close finished work, update in-progress items
4. Commit work locally. Push only when the user asks for it (or explicitly approves) — especially when other agents are working in the same tree.
5. **Hand off** - Provide context for next session
<!-- END BEADS INTEGRATION -->

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

When the user types `/graphify`, use the installed graphify skill or instructions before doing anything else.

Rules:
- For codebase questions, first run `graphify query "<question>"` when graphify-out/graph.json exists. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- Dirty graphify-out/ files are expected after hooks or incremental updates; dirty graph files are not a reason to skip graphify. Only skip graphify if the task is about stale or incorrect graph output, or the user explicitly says not to use it.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).

## Agent Toolchain

Rules that apply to ALL AI agents (Claude Code, OpenCode, Hermes, Cursor, ...) working in this repo.

- **Project skills** live in `.claude/skills/` — every agent should load `gtnh-platform` (SKILL.md) first: it's the operating manual (truth hierarchy, task lifecycle, parallel-agent discipline, verification, session close). Claude Code and OpenCode pick these up automatically; Hermes via `skills.external_dirs`.
- **ICM persistent memory — MANDATORY**: `icm recall "<query>"` before starting work; `icm store -t <topic> -c "..." -i <importance>` when: error resolved, architecture decision made, user preference discovered, significant task completed, or ~20 tool calls without a store. Do NOT store trivia already documented in this file.
- **Code navigation**: codegraph MCP daemon is running (`.codegraph/`, SQLite+WASM, zero infra) — use `codegraph_explore "<query>"` for symbol/relationship questions before raw grep. Knowledge graph: `graphify query "<question>"` (see graphify section).
- **Parallel agents**: OpenCode agents in `.claude/worktrees/` may commit to `main` during your session. Always check fresh `git status` / `git log --oneline -5` / `git reflog -5` before answering anything about repo state. Run `git pull --rebase` before touching shared zones: `src/protocol/`, `data/registry/`, `data/recipes/`, `CMakeLists.txt`, `conanfile.txt`.
- **Task tracking**: use `bd` (beads) for ALL task tracking — never markdown TODO lists (see Beads section above).

Preserve the existing order of ACCEPTED status checks in event handlers unless explicitly directed to reorder them.
When analyzing logs, prefer command-line tools (grep, awk, jq) over reading entire files into context to minimize token consumption