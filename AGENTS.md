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

Distributed Minecraft-style platform with C++ performance core + Go sidecars. Binary protocol (FlatBuffers + Asio TCP) connects 9 services via message router.
It's ONLY linux project, no windows support

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

## COMMANDS

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j

# Run (order matters)
./build/routerd
./build/chunkd
./build/gatewayd
./build/simcored
./build/client
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

## MVP PLAN (6 services, 2 days)

### Day 1: World + Network
- [ ] MessageRouter (Go): pub/sub on TCP, 50 lines
- [ ] ChunkStore (C++): Chunk (32³ flat array), SetBlock/GetBlock, LMDB save/load
- [ ] WorldGenerator (C++ stub): flat world (grass, stone, air)
- [ ] Gateway (C++ asio): TCP accept, 1 client = 1 coroutine, sends ChunkSnapshot on connect. No LZ4, no rate limit — just pipe.

### Day 2: Simulation + Client
- [ ] SimulationCore (C++): subscribes to BlockChanged. Hardcoded 3×3×3 pattern. On match → creates MultiblockController, sends SetBlockMeta to ChunkStore
- [ ] GameClient (C++): bgfx, FPS camera, DDA raycast, ImGui debug overlay. LMB places block → PlayerAction → Gateway → SimulationCore → ChunkStore
- [ ] MetaDB (Go stub): SQLite, table `players(id, x, y, z)`. Writes position on logout

### MVP Result
6 processes. Run them, walk flat world, place blocks. If 3×3×3 special block pattern → ImGui shows "Multiblock #1 active". All communicate via TCP through MessageRouter. Kill SimulationCore — world persists, but new multiblocks don't form.

### Startup Order
```bash
./build/routerd    # Internal pub/sub
./build/chunkd     # World persistence
./build/gatewayd   # TCP gateway
./build/simcored   # Simulation
./build/client     # Game client
```

## MVP ARTIFACTS

### CMakeLists.txt
```cmake
cmake_minimum_required(VERSION 3.20)
project(GTNHPlatform CXX C)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Conan
if(DEFINED ENV{CONAN_HOME})
    set(Conan_FOUND TRUE)
endif()

# Conan imports all dependencies via CMakeDeps
# No vcpkg needed — Conan handles everything
```

### FlatBuffers Schemas (src/protocol/)

- **core.fbs**: Vec3i, Vec3f, ItemStack, Block, Chunk, PlayerAction, EntitySnapshot, CraftRequest, CraftResponse, GridUpdate, BlockAck
- **gateway.fbs**: GatewayPayload union
- **chunkstore.fbs**: GetBlock/SetBlock/GetChunk/SaveChunk RPC
- **simcore.fbs**: BlockChangedReq, MatchPatternReq, TickReq
- **recipe.fbs**: MachineType enum (0=NONE..5=CHEMICAL_REACTOR), CheckRecipeReq, CraftReq, EvaluateConditionsReq
- **entity_state_store.fbs**: GetEntityStateReq/Resp, SetEntityStateReq/Ack
- **tile_entity_store.fbs**: TileEntity save/load
- **item_registry.fbs**: ItemRegistry sync
- **chunk.fbs**: ChunkData wire format

### Interface stubs (C++ headers)
```cpp
// Note: Early MVP interface stubs (IChunkStore, ISimulation, IGateway) 
// have been superseded by FlatBuffers RPC protocol.
```

### go.mod
```go
module github.com/gtnh/platform

go 1.22

require (
    github.com/google/flatbuffers v24.3.25+incompatible
    github.com/mattn/go-sqlite3 v1.14.22
)
```

### CMake + Conan setup
```cmake
# Conan handles all dependencies via CMakeDeps
# No vcpkg section needed
```

## TODO

- [ ] CraftResponse UI feedback in WorkbenchWindow
- [ ] ConditionEvaluator with real MachineState (currently empty)
- [ ] Server-authoritative grid state via TileEntityStore RPC
- [ ] Inventory chain: SimulationCore → MetaDB → gateway → client
- [ ] Drag-and-drop state machine in SlotGrid
- [ ] macerator.json recipes
- [ ] RecipeManager standalone RPC service (protocol defined, not deployed)

---

**Generated**: 2026-06-07 | **Branch**: main

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
