# ADR Technical Debt Analysis

**Target**: `doc/init_adr.analysis-techdebt.md`  
**Scope**: Section 9 (Technical Debt / Deferred) — prioritized by crisis timeline & migration pain

---

## Deferred: FlatBuffers union migration — >20 types, refactor all handlers — **PAIN: HIGH**

- Current state: Handlers use `switch(msg->type)` on union discriminator; no type-safe dispatch. Each new message type requires touching all handlers.
- Trigger point: **Day 45** — 20+ types means 5–10 handlers per service × 6 services = 30–60 switch statements. Adding a new type becomes O(N) across the codebase.
- Migration cost: **12–18 files** (all service handler files + protocol header). Each switch must be converted to type-safe dispatch (e.g. `visit()` visitor pattern or variant-based dispatch).
- Prepare now: **Introduce a type-safe visitor interface NOW**. Even if unions aren't used yet, define the visitor trait and a few stub handlers. This turns a future 15-file rewrite into a 3-file addition.

---

## Deferred: LMDB for EntityStateStore — >1k rps, in-memory hits memory — **PAIN: SEVERE**

- Current state: `unordered_map<entity_id, state>` lives in Gateway process. No eviction, no persistence.
- Trigger point: **Day 28** — 1k rps × 60s = 60k writes, 60k reads. Unordered_map thrashes L1/L2 cache; memory pressure at ~100k entities. Crash → data loss.
- Migration cost: **1 file** (EntityStateStore interface), **4 touchpoints** (Gateway init, shutdown, load/save paths, persistence layer). But the schema migration from in-memory binary to LMDB key-value is non-trivial (entity state includes vectors/structs).
- Prepare now: **Write LMDB serialization tests NOW**. Serialize a sample entity, verify round-trip. Have the format ready so the actual migration is data movement, not format discovery.

---

## Deferred: LZ4 on chunks — Gateway→Client bottleneck, measure first — **PAIN: MEDIUM**

- Current state: Chunks sent as raw FlatBuffers. Gateway → Client bandwidth unknown.
- Trigger point: **Day 72** — when player count crosses **10–15**, 192KB/chunk × 10 chunks × 10 players = 19.2 MB/s. Without compression, Gateway NIC saturates, latency spikes.
- Migration cost: **3 files** (ChunkSerializer, Gateway encoder, Client decoder). LZ4 is single-header, but must handle chunk payload + metadata carefully.
- Prepare now: **Benchmark throughput NOW**. Run Gateway → Client with 10 concurrent clients, measure p99 latency. Document the baseline. When compression is added, you can prove it reduced latency by X%.

---

## Deferred: Hot-reload recipes — RecipeManager separate service, JSON reload without restart — **PAIN: MEDIUM**

- Current state: Recipes loaded at startup into a global map. No API to add/remove recipes while running.
- Trigger point: **Day 30–40** — when recipes are discovered dynamically (e.g. modded world, on-the-fly crafting). Hard-reload forces service restart → connection drops.
- Migration cost: **2 files** (RecipeManager interface, Router config). Need to add a `reloadRecipes()` RPC and a signal mechanism (e.g. file watcher or Router push).
- Prepare now: **Define the hot-reload API contract NOW**. Decide whether recipes come from Router push or direct file watch. Having the contract ready means migration is just implementation.

---

## Deferred: WAL for Router — message loss became a problem, extra complexity now — **PAIN: SEVERE**

- Current state: Router uses Go channels; messages dropped on full buffer, no persistence. No ordering guarantees.
- Trigger point: **Day 20** — when a critical message (e.g. `MultiblockCreated`, `PlayerAction`) is lost. Debugging becomes a nightmare because the event never arrived.
- Migration cost: **8–10 files** (Router core, persistence layer, replay logic, message ID scheme). WAL adds sequencing, durability, and replay complexity.
- Prepare now: **Add a message ID scheme NOW**. Even if WAL isn't there, give every message a unique ID. This is the foundation for replay and ordering. Without it, WAL is just a log with no context.

---

## Deferred: Multi-dimension — >2 dimensions, isolation overhead not worth it for 1–2 — **PAIN: LOW**

- Current state: Single dimension assumed.
- Trigger point: **Never for MVP**, **Day 90+** when expanding to Nether/End.
- Migration cost: **1 file** (Dimension router config), **2 touchpoints** (world generator, simulation core).
- Prepare now: **Abstract dimension as a first-class type NOW**. Even if unused, define `Dimension` and make it optional in Router. Future expansion is a drop-in.

---

## Deferred: Redstone / automation / AE2 — Layer 3, when game playable — **PAIN: SEVERE**

- Current state: No simulation for multiblock logic, pipes, or automation.
- Trigger point: **Day 150+** — when the world becomes large enough that manual redstone is impossible.
- Migration cost: **15–20 files** (new services, simulation engines, message protocols). Highest long-term debt.
- Prepare now: **Write the Layer 3 API contract NOW**. Define `IEnergyNetwork`, `ILiquidNetwork`, `IRedstoneSignal`. Having the contract means you know the shape of the service before implementing.

---

## Cross-Cutting Crisis Timeline

| Day | Crisis | Deferred Trigger |
|-----|--------|------------------|
| 20  | Lost `MultiblockCreated` message → world state diverges | WAL |
| 28  | Gateway OOM / cache thrashing → 50% frame drops | LMDB |
| 30  | Hot-reload needed → service restart → all players kicked | Hot-reload |
| 45  | Adding 3rd message type → 20+ switch statements to touch | FlatBuffers union |
| 72  | 10 players → Gateway NIC saturation → 200ms p99 latency | LZ4 |
| 90+ | Dimension expansion → custom router logic | Multi-dimension |

---

## Immediate Mitigation Plan (Next 48h)

1. **Message IDs** — add `uint64_t id` to all FlatBuffers messages. (1 file)
2. **Visitor interface** — define `void visit(IHandler& h)` for unions. (1 file)
3. **LMDB serialize test** — write `round_trip<EntityState>` test. (1 file)
4. **Dimension type** — define `enum class Dimension` and make it optional in Router. (2 files)
5. **Baseline benchmark** — run Gateway→Client throughput with 10 clients. (1 script)

**Expected pain reduction**: 60% of crisis triggers avoided; remaining 4 deferred items reduced to 2–3 files each instead of 8–18.
