# Project Context

## Purpose

Distributed Minecraft-style platform with a C++ performance core and Go sidecars. 9 microservices communicate over TCP via a unified binary protocol (FlatBuffers + Asio). The world is divided into 32³ chunks; the ChunkStore is the single source of truth for block data, while the SimulationCore owns ECS entities (multiblocks, mobs, pipes). A MessageRouter (Go) handles internal pub/sub with MQTT-style topic matching.

## Tech Stack

### Languages
- **C++26** — hot-path services (Gateway, ChunkStore, WorldGenerator, SimulationCore, PipeNetwork, SpatialIndex, GameClient)
- **Go 1.22** — sidecars (MessageRouter, MetaDB)

### C++ Libraries (Conan-managed)
| Library | Version | Purpose |
|---------|---------|---------|
| Asio | 1.32.0 | TCP server, async IO, io_uring backend |
| FlatBuffers | 25.9.23 | Zero-copy binary protocol |
| GLM | 1.0.1 | Math (vec3, matrices) |
| EnTT | 3.16.0 | ECS (Entity Component System) |
| miniaudio | 0.11.22 | Audio (footsteps, blocks, UI) |
| spdlog | 1.17.0 | Logging |
| GLFW | 3.4 | Windowing + input |
| Boost | 1.91.0 | Header-only utilities |
| TBB | (system) | Parallel algorithms |
| LMDB | (vcpkg) | Chunk persistence (mmap, zero-copy reads) |
| bgfx | (third_party) | Cross-API rendering |
| imgui | (system) | Debug overlay |

### Go Dependencies
- `database/sql` — SQL abstraction
- `mattn/go-sqlite3` — SQLite driver (CGO)
- `flatbuffers` Go — binary protocol parsing
- Zero external runtime dependencies (stdlib only for MessageRouter)

### Build System
- **CMake 3.20+** — C++ projects
- **Conan** — C++ dependency management (`conanfile.txt`)
- **vcpkg** — LMDB + Wayland dependencies (optional, via `VCPKG_ROOT`)
- **Go modules** — Go services (`go.mod`)

### Protocol
- **FlatBuffers** — single shared schema under `src/protocol/` with namespace `Protocol`
- Wire format: `[4 bytes: big-endian payload size] [payload]`
- MessageRouter wire format: `[4 bytes payload length BE] [1 byte msg type] [payload]`

## Project Conventions

### Code Style
- **C++**: C++26 standard. PascalCase for classes (`IoUringGateway`, `PlayerInterest`), snake_case for members (`listen_fd_`, `router_registered_`). Trailing underscore for private members. `delete` copy constructors/assignment on owning types. `#pragma once` for headers.
- **Go**: Standard Go idioms (`err` checks, `defer`, `sync.Mutex`/`RWMutex`, `atomic` for hot fields). PascalCase for exported, camelCase for unexported. No linter config found yet — follow `gofmt` style.
- No `.clang-format` — rely on IDE defaults. `CMAKE_EXPORT_COMPILE_COMMANDS ON` for LSP/clang-tidy.

### Architecture Patterns
- **Each service = separate process** — communicate via TCP/FlatBuffers through the MessageRouter.
- **ChunkStore = single source of truth** for blocks. Flat 32³ arrays, zero-overhead access.
- **Multiblocks = ECS entities** owned by SimulationCore. ChunkStore stores only `mb_id` references in meta-layer for O(1) lookup.
- **Language boundary**: Hot path (ChunkStore, SimulationCore) = C++ only. Sidecars (MetaDB, MessageRouter) = Go. Future scripting via `IExternalLogic` interface.
- **Deferred decisions**: Use interfaces (`IChunkGenerator`, `IClientView`) to swap implementations without breaking consumers.
- **Event-driven**: `BlockChangedEvent` published by ChunkStore → consumed by SimulationCore.
- **Zero-copy**: FlatBuffers data parsed in-place from TCP receive buffers.
- **Service discovery**: MessageRouter maintains registry of connected services and their topics.

### Testing Strategy
- No formal testing framework established yet. Some CLI test files exist (e.g., `chunk_store_cli_test.cpp`). Tests should be added per service.
- Manual integration via startup order: `routerd → chunkd → gatewayd → simcored → client`.

### Git Workflow
- Trunk-based development on `main` branch.
- Commit messages: concise, imperative mood.
- Reference: `AGENTS.md` in project root for AI context; per-service `AGENTS.md` for service-specific knowledge.

## Domain Context

- **Chunk format**: 32³ blocks × (uint16 id + uint8 meta + uint32 mb_id) = 192 KB per chunk, fits in L3 cache.
- **Chunk coordinates**: cx, cy, cz (not block coordinates). Each chunk covers 32×32×32 blocks.
- **CAS-based block placement**: Client sends `expected_block_id` for optimistic concurrency. Server rejects with `CONFLICT` if state changed.
- **Multiblock formation flow**: Client places block → Gateway → SimulationCore pattern checks → ChunkStore SetBlockMeta → `MultiblockCreatedEvent` published.
- **Chunk unload protocol**: ChunkStore asks SimulationCore "can I unload?" — Simulation checks if anchor is inside chunk (serialize & release) or outside (hold).
- **PipeNetwork** runs separately from SimulationCore, solving flow graphs per tick. Caches results if network unchanged for 5 seconds.
- **Topic patterns**: MessageRouter uses MQTT-style wildcards (`+` = one segment, `#` = trailing segments).
- **Service ports**: Gateway (TCP 25565), MessageRouter (TCP 4000), MetaDB (TCP 5005).

## Important Constraints

- ❌ **No JSON parsing in Gateway** — must be zero-copy binary only (FlatBuffers).
- ❌ **No Go for ChunkStore or SimulationCore** — GC pauses unacceptable on hot path.
- ❌ **No breaking multiblock across chunk boundaries** without `SetBlockMeta` coordination.
- ❌ **No storing multiblock controllers in ChunkStore** — SimulationCore owns them.
- ❌ **No gRPC** — overhead for internal pub/sub; Go channels + FlatBuffers are lighter.
- ❌ **No ZeroMQ** — C dependency breaks Go purity.
- ❌ **No Lua/Python mod runtime yet** — mods via C++ `.so`/`.dll` with `dlopen` for now.
- ❌ **No type error suppression** (`as any`, `@ts-ignore`) — not applicable (C++/Go), but same rigor applies: no casting away safety without justification.
- ✅ Conan for C++ deps, vcpkg optional for LMDB/Wayland.

## External Dependencies

### Runtime Services
| Service | Port | Language | Responsibility |
|---------|------|----------|----------------|
| MessageRouter | 4000 | Go | Internal pub/sub, heartbeat, service discovery |
| MetaDB | 5005 | Go | Player saves, inventories via SQLite |
| Gateway | 25565 | C++ | TCP gateway, interest management, client relay |

### Infrastructure
- **Conan** (C++ package manager) — all C++ library dependencies
- **vcpkg** (optional) — LMDB, Wayland
- **CMake** — build orchestration
- **Go toolchain** — MessageRouter, MetaDB builds
- **FlatBuffers compiler (`flatc`)** — schema compilation to C++/Go stubs
