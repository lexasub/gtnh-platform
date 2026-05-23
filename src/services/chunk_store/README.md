# ChunkStore

**Single source of truth for block data**. 32³ flat arrays + LMDB persistence.

## Architecture

```
Chunk (32³)
├─ Layer 0: blocks[]  (16-bit, 0=air)
├─ Layer 1: meta[]    (8-bit, orientation)
└─ Layer 2: multiblockIDs[] (32-bit, 0=free)
```

## Core Principles

1. **Each service = separate process**. Communicate via unified binary protocol (FlatBuffers + Asio TCP).
2. **ChunkStore = single source of truth** for blocks. Flat arrays, zero-overhead.
3. **Multiblocks = ECS entities**, not chunk parts. `multiblockIDs` array enables O(1) lookup.
4. **Language boundaries**: Hot path = C++ only. Sidecars (Go/Python/Java) via `IExternalLogic` interface.
5. **Deferred decisions**: Use interfaces (`IChunkGenerator`, `IClientView`) to swap implementations without breaking consumers.

## Key Components

- **chunk.go**: Flat arrays, 192 KB per chunk
- **lmdb.go**: Zero-copy mmap persistence
- **spatial.go**: ChunkCoord → set<<mbID>> index

## Quick Start

```cpp
// Zero-overhead access
uint16_t id = chunk->blocks[32*32*32];  // O(1)
```

## Notes

- 192 KB per chunk fits L3 cache
- Chunk format: 32 KB + 32 KB + 128 KB