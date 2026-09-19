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

**Generated**: 2026-09-19

## OVERVIEW

Distributed Minecraft-style platform with C++ performance core + Go sidecars. Binary protocol (FlatBuffers + TCP) connects services via MessageRouter (Go pub/sub broker).
Linux-only project. No Windows/macOS support.

## STRUCTURE

```
src/
├── src/apps/                  # 9 runnable daemons (assembly points)
│   ├── gateway/               # TCP gateway, io_uring, interest mgmt
│   ├── chunk_store/           # LMDB-backed block storage, io_uring
│   ├── entity_state_store/    # Entity state persistence, TCP RPC :5200
│   ├── game_client/           # bgfx render, ImGui, input, physics
│   ├── meta_db/               # Player saves, quests, inventories (Go)
│   ├── pipe_network/          # Energy/fluid/item flow graphs
│   ├── recipe_manager/        # Standalone recipe RPC service
│   ├── simcore/               # ECS, multiblocks L2/L3, quests, 20 Hz tick
│   └── world_generator/       # Terrain + ore/tree gen (library, no binary)
├── src/engine/                # Shared engine layer
│   ├── net/                   # gtnh-net: io_uring connections, router client, frame codec
│   ├── registry/              # Registry, ItemId, coords, OpenHashMap
│   ├── sim/                   # ISystem, SimulationEngine, PatternLibrary, ECS components
│   ├── storage/               # Header-only storage interfaces (IEntityStateStorage, …)
│   └── utils/                 # metrics
├── src/game/                  # Gameplay logic extracted from simcore
│   ├── machines/              # Boiler/EBF/LCR/Generator/Explosion/Coolant systems
│   ├── mining/                # Drill, BatteryBuffer, CreativeGenerator
│   ├── quests/                # QuestData/QuestGraph/QuestManager
│   └── recipes/               # RecipeManager, ConditionEvaluator, ItemRegistry
├── src/common/                # GatewayMsg wire constants, ResourcePort, SlotContainer
├── src/content/               # Content registry + data/ (recipes, items, quests, textures)
└── src/protocol/              # FlatBuffers schemas (14 .fbs, namespace Protocol)
```

## SERVICES

| Service | Subdir | Language |
|---------|--------|----------|
| MessageRouter | `src/apps/message_router/` | Go |
| Gateway | `src/apps/gateway/` | C++ |
| ChunkStore | `src/apps/chunk_store/` | C++ |
| WorldGenerator | `src/apps/world_generator/` | C++ (library) |
| SimCore | `src/apps/simcore/` | C++ |
| PipeNetwork | `src/apps/pipe_network/` | C++ |
| SpatialIndex | `src/apps/spatial_index/` | C++ (STUB, not built) |
| EntityStateStore | `src/apps/entity_state_store/` | C++ |
| MetaDB | `src/apps/meta_db/` | Go |
| RecipeManager | `src/apps/recipe_manager/` | C++ (RPC service) |
| Validation | `src/apps/validation/` | C++ (not in default build) |
| GameClient | `src/apps/game_client/` | C++ |

Key engine/game code: net `src/engine/net/`, storage interfaces `src/engine/storage/`, machines `src/game/machines/` (incl. MachineRegistry), recipes `src/game/recipes/`, quests `src/game/quests/`.

## WHERE TO LOOK

| Task                      | Location                      | Notes                              |
|---------------------------|-------------------------------|------------------------------------|
| Binary protocol schema    | `src/protocol/`                       | 14 FlatBuffers `.fbs` files; wire protocol = C++ `GatewayMsg` constants (1–51, 1-based) — `GatewayPayload` union in gateway.fbs is stale |
| Internal message routing  | `src/apps/message_router/`           | Go channels, pub/sub topics        |
| Client connections        | `src/apps/gateway/`                  | TCP accept, interest management    |
| Block data storage        | `src/apps/chunk_store/`              | LMDB, chunk serialization          |
| Terrain generation        | `src/apps/world_generator/`          | Noise functions, biomes            |
| ECS, multiblocks, mobs    | `src/apps/simcore/`                  | EnTT, pattern matching             |
| Machine behavior (EBF, boiler, …) | `src/game/machines/`         | Extracted gameplay systems         |
| Networking library        | `src/engine/net/`                    | io_uring connections, frame codec, router client |
| Headless Gateway tests    | `test/integration/`, `tools/gateway_cli/`, `docs/gateway-headless-client.md` | TCP/FlatBuffers client without GUI |
| Energy/liquid networks    | `src/apps/pipe_network/`             | Graph algorithms, flow solving     |
| Spatial queries           | `src/apps/spatial_index/`            | STUB — not implemented, not built  |
| Entity state persistence  | `src/apps/entity_state_store/`       | LMDB-backed, TCP RPC port 5200     |
| Player saves              | `src/apps/meta_db/`                  | SQLite, transactional saves        |
| Quest system              | `src/game/quests/` + `src/apps/meta_db/` | QuestGraph/QuestManager, quest.fbs protocol, QuestBookWindow |
| Crafting recipes          | `src/content/data/recipes/`          | YAML files per machine type (15 files) |
| Item registry             | `src/content/data/registry/`         | items.csv, items.db, machines.yaml, ores.json, cables/pipes/fluids CSVs |
| Recipe logic              | `src/game/recipes/` + `src/apps/recipe_manager/` | YAML recipes, ConditionEvaluator |
| Texture atlas system      | `doc/texture-atlas-format.md`, `src/content/data/textures/`, `tools/generate_texture_mappings.py` | Additive scanner, dry-run default, `--apply` append-only |
| Rendering, input, audio   | `src/apps/game_client/`              | bgfx, GLFW, ImGui                  |
| Architecture topology     | `doc/c4/`                            | Authoritative C4 diagrams          |

## CONVENTIONS

- **FlatBuffers**: Single schema across all services (`namespace Protocol`)
- **Event-driven**: `BlockChanged` published by ChunkStore → caught by SimCore
- **Language boundaries**: Hot path = C++ only. Sidecars = Go/Python via `IExternalLogic`
- **Zero-copy**: Chunk data flows FlatBuffer → LMDB mmap → TCP send buffer

## ANTI-PATTERNS

- ❌ Breaking multiblock across chunk boundaries without `SetBlockMeta`
- ❌ Using Go for ChunkStore/SimCore (GC pauses unacceptable)
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
```

Note: root `README.md` and `run.sh` still reference the old `src/services/*` layout and are stale; trust the source tree and this file. `run.sh` flags: `--build-dir`, `--db-dir`, `--resolution`, `--all`, `--no-client`.

**Run** (order matters; binary names as built):

1. `routerd` — MessageRouter (Go, :4000)
2. `chunkd` — ChunkStore (C++, :5001)
3. `entitystated` — EntityStateStore (C++, :5200)
4. `gatewayd` — TCP gateway (C++, :7777 ctrl + :7778 bulk)
5. `simcored` — SimCore (C++, 20 Hz tick)
6. `metadbd` — MetaDB (Go, :5005)
7. `pipenetworkd` — PipeNetwork (C++)
8. `gameclientd` — Game client (C++, bgfx)

**If build fails**: Check `conan install` was run. See README.md for Conan setup.

Always compile and verify changes incrementally after each small logical chunk, never batched, to avoid hitting the $1/day API budget during final build checks.

**Tests**:
```bash
cd cmake-build-debug && ctest --output-on-failure -j$(nproc)
```
Integration tests (Go): `test/integration/`. Load tests: `test/loadtest/`.

## NOTES

- Chunk format: 32 KB + 32 KB + 128 KB = 192 KB per chunk
- Multiblock ID stored in meta-layer (O(1) lookup without scanning world)
- MessageRouter uses Go channels — 100k concurrent pub/sub topics are cheap
- Real content data root = `src/content/data/` (root `data/` and `src/data/` are legacy/tooling only)

## LIBRARY DECISIONS

### C++ Stack
| Library | Purpose | Why |
|---------|---------|-----|
| **Asio** | TCP server, async IO, io_uring backend | Zero-copy recv→FlatBuffer, coroutine-friendly (C++20), standard, production-grade |
| **FlatBuffers C++** | Binary protocol | Single schema, `GetRoot<Message>()` zero-copy parsing, no allocations |
| **EnTT** | ECS (Entity Component System) | Fastest C++ ECS, sparse sets, O(1) iteration, cache-miss friendly |
| **LMDB** | Chunk persistence | Read-optimized, mmap, zero-copy reads, ACID, embedded (no separate process) |
| **fastnoise2** | Terrain generation | SIMD-friendly 3D noise, fractal Brownian |
| **GLM** | Math (vec3, matrices, noise coords) | Header-only, consistent syntax across services |
| **spdlog/fmt** | Logging | Header-only, async mode, production-grade formatting |
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
- **RocksDB** — write-optimized, unnecessary overhead for chunk reads
- **JSON parsing in Gateway** — forbidden. Must be zero-copy binary only.

## SERVICE BOUNDARIES (critical)

### ChunkStore vs SimCore
**ChunkStore** = dumb storage. Only knows `block_id + meta + mb_id`. Never understands "electrolyser" or "pipe".

**SimCore** = owns Multiblock Controllers. Holds `entt::registry` with `MultiblockController{mb_id, anchor, blocks[...]}`.

When multiblock forms:
```
Client → Gateway → SimCore
                        ↓ (RPC: GetBlock in pattern radius)
                    ChunkStore
                        ↓ (match found)
                Create MultiblockController in ECS
                        ↓ (RPC: SetBlockMeta for ALL pattern blocks)
                    ChunkStore (writes mb_id into chunk meta-layer)
```

On chunk unload:
1. ChunkStore marks chunk `pending_unload`
2. Asks SimCore: "here is list of mb_id in this chunk, can I unload?"
3. Simulation checks anchor:
   - **anchor INSIDE chunk** → serializes MB to MetaDB, returns `release`
   - **anchor OUTSIDE chunk** → returns `hold` (MB active, keep chunk in memory)
4. ChunkStore unloads **only on `release`**

### PipeNetwork separate from SimCore
Simulation reports: "network #3: 5 pipes, 2 inputs, 3 outputs".
PipeNetwork solves graph per tick, returns `flow_map`.
If network unchanged 5 seconds → skip tick (cache).
Can run 2 instances (one per dimension) without interference.

### EntityStateStore vs MetaDB
**EntityStateStore** (C++): Persistent state for world-bound entities (tile entities, machine state). LMDB-backed. Topics: entity.state.get/set, TCP RPC port 5200.

**MetaDB** (Go): Player-bound data (inventories, position, stats). SQLite. Connected to MessageRouter via router client.

## TODO

- [ ] Pause menu / settings window in game client (missing)
- [ ] Sound: miniaudio in conanfile, no audio code yet
- [ ] SpatialIndex: implement R-tree/Octree (currently 2-line stub, not built)
- [ ] Dedicated Drill UI window (only tooltip so far)
- [ ] Resolve GatewayMsg C++ constants vs FlatBuffers `GatewayPayload` union divergence
- [ ] Server-authoritative grid state via TileEntityStore RPC
- [ ] Refresh stale root README.md and run.sh for the src/apps + engine layout

---

**Generated**: 2026-09-19 | **Branch**: main
<!-- BEGIN BEADS INTEGRATION v:1 profile:minimal hash:46cd31e7 -->
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

**Architecture in one line:** issues live in a local Dolt DB; sync uses `refs/dolt/data` on your git remote; `.beads/issues.jsonl` is a passive export. See https://github.com/gastownhall/beads/blob/main/docs/core-concepts/sync-concepts.md for details and anti-patterns.

## Agent Context Profiles

The managed Beads block is task-tracking guidance, not permission to override repository, user, or orchestrator instructions.

- **Conservative (default)**: Use `bd` for task tracking. Do not run git commits, git pushes, or Dolt remote sync unless explicitly asked. At handoff, report changed files, validation, and suggested next commands.
- **Minimal**: Keep tool instruction files as pointers to `bd prime`; use the same conservative git policy unless active instructions say otherwise.
- **Team-maintainer**: Only when the repository explicitly opts in, agents may close beads, run quality gates, commit, and push as part of session close. A current "do not commit" or "do not push" instruction still wins.

## Session Completion

This protocol applies when ending a Beads implementation workflow. It is subordinate to explicit user, repository, and orchestrator instructions.

1. **File issues for remaining work** - Create beads for anything that needs follow-up
2. **Run quality gates** (if code changed) - Tests, linters, builds
3. **Update issue status** - Close finished work, update in-progress items
4. **Handle git/sync by active profile**:
   ```bash
   # Conservative/minimal/default: report status and proposed commands; wait for approval.
   git status

   # Team-maintainer opt-in only, unless current instructions forbid it:
   git pull --rebase
   bd dolt push
   git push
   git status
   ```
5. **Hand off** - Summarize changes, validation, issue status, and any blocked sync/commit/push step

**Critical rules:**
- Explicit user or orchestrator instructions override this Beads block.
- Do not commit or push without clear authority from the active profile or the current user request.
- If a required sync or push is blocked, stop and report the exact command and error.
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
- **Parallel agents**: OpenCode agents in `.claude/worktrees/` may commit to `main` during your session. Always check fresh `git status` / `git log --oneline -5` / `git reflog -5` before answering anything about repo state. Run `git pull --rebase` before touching shared zones: `src/protocol/`, `src/content/data/registry/`, `src/content/data/recipes/`, `CMakeLists.txt`, `conanfile.txt`.
- **Task tracking**: use `bd` (beads) for ALL task tracking — never markdown TODO lists (see Beads section above).

Preserve the existing order of ACCEPTED status checks in event handlers unless explicitly directed to reorder them.
When analyzing logs, prefer command-line tools (grep, awk, jq) over reading entire files into context to minimize token consumption