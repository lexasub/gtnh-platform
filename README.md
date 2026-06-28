# GTNH Platform

**A from-scratch voxel game engine and simulation platform inspired by GregTech: New Horizons.**

Not a mod — a standalone C++ implementation with distributed architecture
(ECS simulation, binary protocol, 9 services). Part platform for experimenting
with GTNH-scale mechanics, part playable game with world, machines, pipes,
crafting, and electric tools. (Gtnh inspired game in future)

Built with C++ performance core + Go sidecars. Binary protocol (FlatBuffers + Asio TCP).

## Git History & Contributing

**Git history**: The current commit is the initial one. The development history was volatile (architecture and protocol changed multiple times), so I'll squash into a single clean commit once I set up the remote. If you need a draft branch with the full messy history (bad commit messages, broken intermediate states) — I can grant access separately.

**Looking for contributors.** Areas that need work:

| Area | Scope / keywords |
|------|-----------------|
| **Inventories** | EntityStateStore persistence, drag-and-drop state machine, WorldContainerInventory |
| **Crafting** | RecipeManager, macerator, WorkbenchStateManager, server-authoritative grid |
| **Generators** | CreativeGenerator config, heat transfer, heat_generator neighbor propagation |
| **Heat transfer** | Boiler, overheat, water→steam, explosion, thermal dynamics |
| **Pipes/cables** | PipeNetwork BFS, CableGraph, CableLoss, overheat, item/fluid transport |
| **UI** | MachineWindow, Drill UI, inventory drag-and-drop, ImGui widgets |
| **Assets** | Textures, models, sprites for items, blocks, and machines |
| **Questbook** | Quest library, quest data, completion tracking, rewards |
| **Game modes** | Survival (no mobs — ore gen, gating, tools), Creative (build mode), Spectator (current) |
| **Protocol** | FlatBuffers enum/constant hardcodes → generated code, GatewayMsg 1-based vs 0-based fix |
| **Tests** | Contract/integration tests: protocol frames, router pub/sub flows, RPC boundaries, service handoff |

Overall **everything works**, but there are bugs — code was written fast, architecture changed on the fly. Many places have **hardcoded values** that need architectural workarounds.

**Architecture discussions welcome.** The process:
1. Open a PR with architecture change proposals for a specific component
2. I analyze it, we discuss, refine
3. Once architecture is agreed upon — we create a task, done by me and/or you

The goal: understanding and fixing a component should require fewer changes and less context.

## Architecture

> **Note:** This diagram is approximate, incomplete, and may be inaccurate. For the authoritative topology, see the [C4 diagrams](doc/c4/) — especially `level2-container.puml` and `level3-*.puml`.


```
                 ┌─────────────┐
                 │   Client    │
                 │  (bgfx)     │
                 └──────┬──────┘
                        │  TCP/FlatBuffers (ctrl :7777 + bulk :7778)
                        │  PlayerAction / ChunkData / CraftRequest / ...
                 ┌──────▼──────┐
                 │  Gateway    │
                 │  (C++)      │
                 │  IoUring    │
                 └──────┬──────┘
                        │  pub/sub: Register / Subscribe / Publish
                        │  topics: player.* / world.* / recipe.* / ...
                 ┌──────▼──────────┐
                 │  MessageRouter  │ ◄── Go :4000, pub/sub broker
                 │  (Go)           │     3 priority levels, heartbeat,
                 └──────┬──────────┘     service discovery
                        │
        ┌───────────────┼───────────────┬───────────────┐
        │               │               │               │
   ┌────▼─────┐   ┌────▼─────┐   ┌────▼─────┐   ┌────▼──────┐
   │ Chunk    │   │Simulation│   │  Pipe    │   │ Entity    │
   │ Store    │   │  Core    │   │ Network  │   │ StateStore│
   │ (C++)    │   │ (C++)    │   │ (C++)    │   │ (C++)     │
   └────┬─────┘   └────┬─────┘   └────┬─────┘   └────┬──────┘
        │               │               │               │
   ┌────▼─────┐   ┌────▼─────┐   ┌────▼────────┐  ┌───▼────────┐
   │ WorldGen │   │ MetaDB   │   │ RecipeMgr   │  │ Spatial    │
   │ (C++)    │   │ (Go)     │   │ (C++)       │  │ Index      │
   └──────────┘   └──────────┘   └─────────────┘  │ (planned)  │
                                                   └────────────┘
```

**Connection topology:**
- **All services** connect to MessageRouter for pub/sub (TCP, FlatBuffers frames).
- **Gateway** additionally accepts external client connections (dual-port: ctrl + bulk).
- **SimulationCore → ChunkStore** has direct RPC for block operations.
- **SimulationCore → EntityStateStore** has direct TCP RPC for entity state.
- **SimulationCore → PipeNetwork** has direct RPC for energy/fluid tick.

## Service Map

| #  | Service          | Language | Responsibility                           |
|----|------------------|----------|------------------------------------------|
| 1  | MessageRouter    | Go       | Internal pub/sub, heartbeat, discovery   |
| 2  | Gateway          | C++      | TCP gateway, interest management         |
| 3  | ChunkStore       | C++      | Block data, LMDB persistence             |
| 4  | WorldGenerator   | C++      | Chunk generation, procedural terrain     |
| 5  | SimulationCore   | C++      | ECS, multiblocks, mobs, 20 Hz tick       |
| 6  | PipeNetwork      | C++      | Energy/liquid flow graphs                |
| 7  | SpatialIndex     | C++      | R-tree/Octree, multiblock queries        |
| 8  | EntityStateStore | C++      | Entity state persistence (LMDB), TCP RPC |
| 9  | MetaDB           | Go       | Player saves, inventories                |
| 10 | GameClient       | C++      | bgfx render, ImGui, input                |

## Key Design Decisions

- **Chunk format**: 32³ flat arrays — 192 KB per chunk, fits L3 cache
- **Multiblocks**: Not chunk-owned. Simulation Service owns controllers; Chunk Store only stores `mbID` references
- **Language split**: Hot path = C++ only. Sidecars = Go/Python via `IExternalLogic`
- **I/O**: **io_uring is the primary async backend** (Linux-native). Epoll/IOCP/kqueue fallbacks are not implemented — if someone wants to port to macOS/Windows, those can be added as compile-time alternatives, but io_uring must remain the primary target for all performance work (may be in future using iouring for gpu,chunk store).
- **Protocol**: FlatBuffers (zero-copy) over TCP (length-prefixed frames). Note: some enum constants are still hardcoded instead of using generated FlatBuffers code — cleanup is an active task.

## Build

### Dependencies (TODO NEED CONTRIBUTE)

Two ways to get dependencies:

**Option A — Conan (recommended, automated):**

[Conan](https://conan.io/) handles most dependencies (Asio, EnTT, spdlog, FlatBuffers, LMDB++, GLM, etc.).

```bash
pip install conan
conan install . --build=missing
```

> **Note:** If Conan registry is unavailable in your region, see Option B.

**Option B — Pre-built (build once, reuse):**

Dependencies that aren't in Conan (bgfx, FastNoise2, cmake-imgui, ImGuizmo) are always built manually. Use the setup script:

```bash
./scripts/build-deps.sh
```

This clones and builds all external dependencies into `$HOME/.gtnh-deps/`. Once built, subsequent `cmake` runs use the cached `.a`/`.so` files — no rebuild on `rm -rf build`.

```bash
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=$HOME/.gtnh-deps
make -j
```

For Conan users, the script is optional — Conan pulls everything automatically (except the four above, which still need manual cloning into `third_party/`).

**Go 1.22+** — for MessageRouter and MetaDB services.

## Quick Start

```bash
# Option A: Conan
conan install . --build=missing

# Option B: pre-built (build once, reuse)
# ./scripts/build-deps.sh

mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j

# Run (order matters)
./build/routerd            # Internal pub/sub
./build/chunkd             # World persistence
./build/gatewayd           # TCP gateway
./build/simcored           # Simulation
./build/pipe_networkd      # Energy/fluid/item transport
./build/client             # Game client
```

## Project Structure

```
src/
├── src/
│   ├── services/
│   │   ├── gateway/           # TCP gateway, interest management
│   │   ├── chunk_store/       # LMDB-backed block storage
│   │   ├── world_generator/   # Chunk generation
│   │   ├── simulation_core/   # ECS, multiblocks, mobs
│   │   ├── pipe_network/      # Energy/liquid flow graphs
│   │   ├── spatial_index/     # R-tree/Octree, multiblock queries
│   │   ├── meta_db/           # Player saves, inventories
│   │   └── game_client/       # bgfx render, ImGui, input
│   └── protocol/              # FlatBuffers schemas (.fbs)
├── build/                     # CMake build directory
└── docs/                      # Service documentation
```

## Status

- ✅ **Core MVP**: 10 services, FlatBuffers protocol, MessageRouter pub/sub
- ✅ **Crafting Pipeline**: Workbench crafting end-to-end (CraftRequest→RecipeManager→CraftResponse), 6 JSON recipe types
- ✅ **PipeNetwork**: BFS energy/fluid/item graphs, CableGraph, MessageRouter integration, overheat/explosion
- ✅ **Electric Tools**: DrillSystem (spiral BFS, progress, energy), BatteryBuffer, wrench side config
- ✅ **Autonomous Mining**: DrillSystem MVP — mining progress, output buffer, energy consumption
- ✅ **Heat/Boiler**: Overheat detection, water→steam conversion, explosion
- 🟡 **Inventory System**: Protocol + MetaDB + EntityStateStore implemented, drag-and-drop + persistence pending
- 🔴 **Multiblocks L2**: SpatialIndex, generic pattern library, EBF/Boiler tick — not started
- 🔴 **Item/Fluid Pipes**: CableGraph wired, actual pipe transport not implemented
- 🔴 **Ore Generation**: WorldGenerator flat only, vein generation not started

See `ROADMAP.md` for details.

---

**Generated**: 2026-06-28 | **Branch**: master