# Performance Bug: Head-of-Line Blocking in io_uring Networking Stack

> **Статус**: Dual TCP channel (ctrl :7777 + bulk :7778) **развёрнут**, но баг **не закрыт**.
> Load test показывает 50% потерю ack на холодном старте (Remaining Issues #1–3 внизу).
> Полное исправление требует request_id в протоколе, увеличения batch limit и rate limiting.
> См. `doc/diff-protocol.md` — BlockAck type_id=5 существует, но полный diff protocol не завершён.

## Symptoms

Player action (block break/place) felt "stuck" for 50-200ms when walking into new chunk territory. Camera moved, but clicks didn't register until chunks finished loading.

## Root Cause

### Single io_uring ring = single-threaded janitor

Gateway had **one** `IoUringContext` with ~256 SQEs shared across:

| Traffic | Frame size | Rate |
|---------|-----------|------|
| Chunk snapshots (→ client) | ~229 KB | Burst on movement |
| Entity snapshots (→ client) | ~144 bytes | 20 Hz |
| PlayerAction (← client) | ~36 bytes | Sporadic |
| BlockAck (→ client) | ~40 bytes | Per action |

**The problem**: client `Poll()` submits writes (SQEs), then processes incoming CQEs. A single `poll()` call processes writes AND reads on the same ring. When a 229 KB chunk lands in the TCP receive buffer, the client's `Poll()` spends most of its budget reading that chunk — the small action/ack frames sit in the kernel's TCP buffer **behind** the chunk data.

Same problem on Gateway: `ctx_client_` handles both sending chunks AND reading actions. A slow chunk write blocks action reads.

This is classic **head-of-line (HOL) blocking** at the io_uring level.

### CQ overflow (earlier attempt)

Before the split, we tried mitigating with:
- Ring depth 256 → 1024
- `IORING_SETUP_CQ_NODROP`
- Batch limit 32 in `start_next_writes()`
- uint32 overflow guard in `enqueue_write()`

This reduced CQ overflow crashes but didn't fix the HOL latency problem.

## Fix: Dual TCP Channel (Ctrl + Bulk)

### Architecture

```
Client                     Gateway
┌──────┐    Ctrl :7777     ┌──────────┐
│      │◄─────────────────►│ ctx_ctrl_│──┐
│ IoU  │   PlayerAction    └──────────┘  │
│ ring │     BlockAck                    │
│ ctrl │                   ┌──────────┐  │
│      │    Bulk :7778     │ ctx_bulk_│──┤
│ IoU  │◄─────────────────►└──────────┘  │
│ ring │   ChunkSnapshot                 │
│ bulk │   BlockUpdate       ┌──────────┐│
└──────┘                    │ctx_router│◄┘
                            └──────────┘
                              Router :4000
```

### Gateway: 3× IoUringContext

| Context | Entries | Handles |
|---------|---------|---------|
| `ctx_ctrl_` | 1024 | TCP accept (7777) + client I/O on ctrl connections |
| `ctx_bulk_` | 1024 | TCP accept (7778) + client I/O on bulk connections |
| `ctx_router_` | 1024 | Router I/O (4000) |

Each context has its own worker thread (`io_uring_wq`). Mutex per client connection guards cross-thread access.

**Topic routing** in `router_read_cb()`:
- **Bulk topics** → `client_bulk_` (tags 6, 7, 300)
  - `world.chunk.loaded`
  - `world.blocks.changed`
  - `entities.*`
  - `simulation.multiblock.*`
- **Ctrl topics** → `client_ctrl_` (tags 4, 5, 200)
  - `player.actions.ack`
  - `player.inventory.update`
  - `world.block_entity.update`

### Client: Dual IoUringClient

```cpp
class NetClient {
    IoUringClient io_uring_ctrl_;   // connected to :7777
    IoUringClient io_uring_bulk_;   // connected to :7778
};
```

- `Poll()` calls both rings
- `OnMessage()` handles ctrl msgs (kBlockAck, kInventoryUpdate, kBlockEntityUpdate)
- `OnBulkMessage()` handles bulk msgs (kChunkSnapshot, kBlockUpdate)
- All outgoing **actions** (clicks, block break/place, chunk requests) → ctrl channel

## Load Test Tool

`test/loadtest/loadtest` — standalone Go binary that stress-tests the action path.

### Usage

```bash
cd test/loadtest
./loadtest -ctrl 127.0.0.1:7777 -rate 50 -duration 10
```

### How it works

1. Connects to ctrl port (7777) via raw TCP
2. Sends `SetBlockAction` frames at configurable rate
3. Parses `BlockAck` responses (CONFLICT or OK)
4. Reports throughput + latency stats

### Protocol (no server changes needed)

Sends `Protocol.SetBlockAction` with:
- `pos`: varied across 32×32 grid (1024 unique positions)
- `player_id`: hardcoded 0
- `from_id / to_id`: CAS semantics — first hit always CONFLICTs (both are 0), second matches

### Results

| Run | Rate | Sent | Recv | % | Latency avg | p50 | p99 |
|-----|------|------|------|---|-------------|-----|-----|
| Cold | 50/s | 500 | 243 | 49% | 0.70 ms | 0.68 ms | 1.14 ms |
| Warm | 50/s | 500 | 505 | 101%¹ | 1.85 ms | 1.94 ms | 3.49 ms |

> ¹ 101% is an artifact: no unique request ID, position-based key collides on retry, double-counting responses.

### Benchmark Instability

Results vary significantly between cold/warm runs because:

| Factor | Effect |
|--------|--------|
| **Cold server** | Chunks generated first time, LMDB page faults, slower processing |
| **Warm server** | Chunks cached in LMDB + OS page cache, faster |
| **Python GC pause** (load test) | Goroutine scheduler variation, timestamp jitter |
| **No unique request ID** | Position-based dedup key collides on retry |

## Remaining Issues

### 1. Missing request_id in protocol

**Problem**: `BlockAck` has no `request_id` field. Client can't match ack to action reliably.

**Impact**:
- Load test uses position as key → collisions on retry
- Real client can't show per-action UI feedback (e.g. "action failed" popup)
- Can't measure true end-to-end latency per action

**Fix**: Add `request_id` (uint32) to both `SetBlockAction` and `BlockAck`. Server echoes `request_id` verbatim.

### 2. start_next_writes() batch limit

**Problem**: Gateway sends acks to client in batches of 32 per `start_next_writes()` call. Under burst (50+ acks/tick), some acks queue up.

**Observation**: 50% packet loss on cold server (243/500) suggests Gateway outbound is the bottleneck. Under load, `start_next_writes()` may not cycle fast enough to drain the write queue.

**Fix**: Increase batch limit, or remove batching entirely (io_uring can handle 1024 SQEs).

### 3. No rate limiting on client actions

**Problem**: Client can send actions faster than SimulationCore's 20 Hz tick. Unprocessed actions queue up on Gateway.

**Fix**: Gateway should throttle actions per player (max N actions per tick) and drop excess with an error ack.

## Files Changed

| File | Change |
|------|--------|
| `src/services/gateway/gateway.h` | 3× IoUringContext, ctrl/bulk split, mutex per client |
| `src/services/gateway/gateway.cpp` | Dual accept, topic routing, dual write dispatch |
| `src/services/gateway/main.cpp` | `--bulk-port 7778` argument |
| `src/services/game_client/Network/NetClient.h` | Dual IoUringClient |
| `src/services/game_client/Network/NetClient.cpp` | Poll both rings, OnBulkMessage |
| `src/services/game_client/GameClient.h` | bulk_port parameter |
| `src/services/game_client/GameClient.cpp` | Init with bulk_port |
| `src/services/game_client/main.cpp` | `--bulk-port 7778` argument |
| `test/loadtest/main.go` | Load test tool |
| `test/loadtest/go.mod` | Go module |
