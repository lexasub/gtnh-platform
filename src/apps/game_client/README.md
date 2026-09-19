# GameClient

**Distributed Minecraft-style platform** with C++ performance core + Go sidecars. Binary protocol (FlatBuffers + Asio TCP) connects 9 services via message router.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        CLIENT                                │
│              (bgfx + ImGui + Asio TCP)                       │
└───────────────────────┬─────────────────────────────────────┘
                        │ TCP (FlatBuffers, compressed chunks)
                        ▼
┌─────────────────────────────────────────────────────────────┐
│  GATEWAY NODE (C++, public TCP port)                         │
│  • TCP accept, auth, heartbeat                              │
│  • 1 client = 1 connection                                  │
│  • Relay: World→Client, Client→Simulation, Simulation→Client│
│  • Zero-copy FlatBuffers parse                               │
└──────┬──────────────────────┬───────────────────────┘
       │ localhost TCP (internal bus)                    │
       ▼                                                    ▼
┌─────────────────────┐                      ┌──────────────────────────────┐
│  WORLD NODE (C++)   │◄────────────────────►│  SIMULATION NODE             │
│  • ChunkStore       │  BlockChanged /      │  • ECS (EnTT): multiblocks,  │
│  • SetBlock/GetBlock│  SetBlockMeta /      │    pipes, mobs, redstone      │
│  • Load/Save (LMDB) │  GetBlock RPC        │  • Pattern matching МБ         │
└─────────────────────┘                      └──────────────────────────────┘
```

## Core Principles

1. **Each service = separate process**. Communicate via unified binary protocol (FlatBuffers + Asio TCP).
2. **ChunkStore = single source of truth** for blocks. Flat arrays, zero-overhead.
3. **Multiblocks = ECS entities**, not chunk parts. `multiblockIDs` array enables O(1) lookup.
4. **Language boundaries**: Hot path = C++ only. Sidecars (Go/Python/Java) via `IExternalLogic` interface.
5. **Deferred decisions**: Use interfaces (`IChunkGenerator`, `IClientView`) to swap implementations without breaking consumers.

## Protocol

FlatBuffers schema (`schema/protocol.fbs`) defines all IPC messages. Length-prefixed frame over TCP.

## Key Components

- **bgfx**: Cross-API rendering (GL/Vulkan/D3D/Metal)
- **GLFW**: Window + input (keyboard, mouse)
- **ImGui**: Debug overlay, menus
- **Asio**: TCP client

## Quick Start

```cpp
// Raycast example
glm::vec3 origin = camera->GetPosition();
glm::vec3 direction = camera->GetForward();
glm::vec3 hit = raycast(origin, direction);
if (hit.valid) {
    playerAction->target_x = hit.x;
    playerAction->target_y = hit.y;
    playerAction->target_z = hit.z;
}
```

## Notes

- Chunk format: 32 KB + 32 KB + 128 KB = 192 KB per chunk
- Multiblock ID stored in meta-layer (O(1) lookup without scanning world)
- MessageRouter uses Go channels — 100k concurrent pub/sub topics are cheap