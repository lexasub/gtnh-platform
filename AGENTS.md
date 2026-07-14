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

**Generated**: 2026-06-07

## OVERVIEW

Distributed Minecraft-style platform with C++ performance core + Go sidecars. Binary protocol (FlatBuffers + Asio TCP) connects 10 services via message router.
Linux-only project. No Windows/macOS support.

## STRUCTURE

```
src/
├── src/
│   ├── services/
│   │   ├── gateway/           # TCP gateway, interest management
│   │   ├── chunk_store/       # LMDB-backed block storage
│   │   ├── world_generator/   # Chunk generation
│   │   ├── simulation_core/   # ECS, multiblocks, 20 Hz tick
│   │   ├── pipe_network/      # Energy/liquid flow graphs
│   │   ├── spatial_index/     # R-tree/Octree, multiblock queries
│   │   ├── meta_db/           # Player saves, inventories
│   │   ├── entity_state_store/ # Entity state persistence, TCP RPC
│   │   └── game_client/       # bgfx render, ImGui, input
│   └── protocol/              # FlatBuffers schemas
├── build/                     # CMake build directory
└── data/                      # Recipes, item registry
└── docs/                      # Service documentation
```

## SERVICES

| Service | Subdir | Language |
|---------|--------|----------|
| Gateway | `src/services/gateway/` | C++ |
| ChunkStore | `src/services/chunk_store/` | C++ |
| WorldGenerator | `src/services/world_generator/` | C++ |
| SimulationCore | `src/services/simulation_core/` | C++ |
| PipeNetwork | `src/services/pipe_network/` | C++ |
| SpatialIndex | `src/services/spatial_index/` | C++ |
| MetaDB | `src/services/meta_db/` | Go |
| EntityStateStore | `src/services/entity_state_store/` | C++ |
| RecipeManager | `src/libs/recipe_manager_lib/` | C++ (shared lib) |
| GameClient | `src/services/game_client/` | C++ |

## WHERE TO LOOK

| Task                      | Location                      | Notes                              |
|---------------------------|-------------------------------|------------------------------------|
| Binary protocol schema    | `src/protocol/`                       | FlatBuffers `.fbs` files           |
| Internal message routing  | `src/services/message_router/`       | Go channels, pub/sub topics        |
| Client connections        | `src/services/gateway/`              | TCP accept, interest management    |
| Block data storage        | `src/services/chunk_store/`          | LMDB, chunk serialization          |
| Terrain generation        | `src/services/world_generator/`      | Noise functions, biomes            |
| ECS, multiblocks, mobs    | `src/services/simulation_core/`      | EnTT, pattern matching             |
| Energy/liquid networks    | `src/services/pipe_network/`         | Graph algorithms, flow solving     |
| Spatial queries           | `src/services/spatial_index/`        | R-tree, multiblock lookup          |
| Entity state persistence  | `src/services/entity_state_store/`   | LMDB-backed, TCP RPC port 5200     |
| Player saves              | `src/services/meta_db/`              | SQLite, transactional saves        |
| Crafting recipes          | `data/recipes/`                      | JSON files per machine type        |
| Item registry             | `data/registry/`                     | items.csv + items.db               |
| Recipe system             | `src/libs/recipe_manager_lib/`       | JSON recipes, ConditionEvaluator    |
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
./cmake-build-release/simcored           # 5. Simulation (C++, 20Hz tick)
./cmake-build-release/meta-dbd           # 6. Player DB (Go, :5005 + :5006)
./cmake-build-release/pipe_networkd      # 7. Energy/fluid transport (C++)
./cmake-build-release/client             # 8. Game client (C++, bgfx)
```

**If build fails**: Check `conan install` was run. See README.md for Conan setup.

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

- [ ] CraftResponse UI feedback in WorkbenchWindow
- [ ] ConditionEvaluator with real MachineState (currently empty)
- [ ] Server-authoritative grid state via TileEntityStore RPC
- [ ] Inventory chain: SimulationCore → MetaDB → gateway → client
- [ ] Drag-and-drop state machine in SlotGrid
- [ ] macerator.json recipes
- [ ] RecipeManager standalone RPC service (protocol defined, not deployed)

---

**Generated**: 2026-07-14 | **Branch**: main

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

**When ending a work session**, you MUST complete ALL steps below. Work is NOT complete until `git push` succeeds.

**MANDATORY WORKFLOW:**

1. **File issues for remaining work** - Create issues for anything that needs follow-up
2. **Run quality gates** (if code changed) - Tests, linters, builds
3. **Update issue status** - Close finished work, update in-progress items
4. **PUSH TO REMOTE** - This is MANDATORY:
   ```bash
   git pull --rebase
   git push
   git status  # MUST show "up to date with origin"
   ```
5. **Clean up** - Clear stashes, prune remote branches
6. **Verify** - All changes committed AND pushed
7. **Hand off** - Provide context for next session

**CRITICAL RULES:**
- Work is NOT complete until `git push` succeeds
- NEVER stop before pushing - that leaves work stranded locally
- NEVER say "ready to push when you are" - YOU must push
- If push fails, resolve and retry until it succeeds
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
