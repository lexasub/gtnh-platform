# Inventory Chain Implementation Plan

**Date**: 2026-06-07
**Scope**: Client → Gateway → MetaDB → SQLite (player-bound data)
**Out of scope**: EntityStateStore (world-bound), PipeNetwork, SpatialIndex

---

## 1. Current Gap Analysis

### 1.1 What Exists

| Component | File(s) | Status | Details |
|-----------|---------|--------|---------|
| **Protocol: `InventoryUpdate`** | `protocol/core.fbs:149` | ✅ Complete | `{player_id, slots:[InventorySlot]}` — wire format for gateway→client pushes |
| **Protocol: `InventoryAction`** | `protocol/core.fbs:167` | ✅ Complete | `{player_id, action_type, source_slot, target_slot, count}` — client→server ops |
| **Protocol: MetaDB RPC** | `protocol/meta_db.fbs` | ✅ Complete | `GetInventoryReq`, `SetInventorySlotReq`, `GetInventorySnapshotReq`, `GetInventoryResp` |
| **Gateway: message types** | `gateway.h:34-35` | ✅ Complete | `kInventoryUpdate=6`, `kInventoryAction=7` |
| **Gateway: ctrl read (kInventoryAction)** | `gateway.cpp:378-383` | ✅ Complete | Verifies `InventoryAction`, calls `on_client_message` (publishes to `player.actions`) |
| **Gateway: router→client (player.inventory.update)** | `gateway.cpp:355-356` | ✅ Complete | Forwards `player.inventory.update` topic as `kInventoryUpdate` to client ctrl |
| **Gateway: subscription** | `gateway.cpp main:87` | ✅ Complete | Subscribes to `player.inventory.update` |
| **Gateway: Integration layer** | `Integration/InventoryIntegration.h/.cpp` | ✅ Complete | Abstract routing between `IPlayerInventoryStorage` + `IEntityStateStorage` |
| **Gateway: Storage interfaces** | `storage_interfaces/IPlayerInventoryStorage.h` | ✅ Interface only | `LoadPlayerInventory`, `SavePlayerInventory`, `GetPlayerPosition`, `SavePlayerPosition` |
| **MetaDB: SQLite schema** | `meta_db/db.go:17-36` | ✅ Complete | `players(id, x, y, z)`, `inventory(player_id, slot, block_id, count)` |
| **MetaDB: CRUD functions** | `meta_db/db.go:85-172` | ✅ Complete | `CreateInventory`, `GetInventory`, `UpdateInventorySlot`, `DeleteInventorySlot` |
| **MetaDB: Player position** | `meta_db/db.go:209-225` | ✅ Complete | `SavePlayerPosition`, `GetPlayerPosition` |
| **MetaDB: FlatBuffer TCP** | `meta_db/flatbuffer_tcp.go` | ✅ Complete | Listener on `:5006`, handles `GetInventoryReq`, `SetInventorySlotReq`, `GetInventorySnapshotReq` |
| **MetaDB: Router client** | `meta_db/router_client.go` | ✅ Complete | Subscribes to `meta_db.inventory.{get,set,snapshot}`, dispatches via `dispatchFlatBufferFrame` |
| **MetaDB: PublishInventoryUpdate** | `meta_db/db.go:175-203` | ✅ Complete | Publishes to `player.inventory.update` topic via router |
| **MetaDB: startup** | `meta_db/main.go` | ✅ Complete | Starts FlatBuffer TCP + router client + legacy JSON listener |
| **NetClient: SendInventoryAction** | `NetClient.h:69-70` | ⚠️ Partial | Signature: `(slot, item_id, count, stack_size, mb_id)` — doesn't match FBS `InventoryAction` schema; missing `action_type`, `source_slot`, `target_slot`, `player_id` |
| **NetClient: InventoryUpdate callback** | `NetClient.h:38` | ✅ Complete | `InventoryUpdateCallback` type + setter + dispatch in `OnMessage` |
| **GameClient: inventory callback wiring** | `GameClient.cpp:79-83` | ✅ Complete | Wires `onInventoryUpdate_` → `uiMgr_.HandleNetwork()` |
| **PlayerInventory: UI rendering** | `PlayerInventory.h/.cpp` | ✅ Complete | Drag-and-drop, hotbar, inventory grid |
| **SlotGrid: component** | `SlotGrid.h/.cpp` | ✅ Complete | Rendering utility |
| **UIManager: HandleNetwork** | `UIManager.cpp:73-77` | ✅ Complete | Dispatches to all windows via `OnNetworkUpdate` |
| **IUIWindow: OnNetworkUpdate** | `IUIWindow.h:54` | ✅ Complete | Virtual method for network message handling |
| **EntityStateStore** | `entity_state_store/` | ✅ Complete | LMDB-backed, TCP port 5200, handles `entity.state.{get,set}` |

### 1.2 What's Missing

| Gap | Impact | Details |
|-----|--------|---------|
| **No `IPlayerInventoryStorage` implementation** | Gateway cannot reach MetaDB | `InventoryIntegration` has the abstract layer but no TCP client class connecting to MetaDB's `:5006`. The concrete class `MetaDBTcpClient` (or `MetaDBRouterClient`) does not exist. |
| **No player login handler** | Inventory never loaded on join | Gateway has no `on_connect` callback. When a client connects, their `player_id` is unknown and no `GetInventoryReq` is sent to MetaDB. Client starts with hardcoded dummy items in `GameClient.cpp:104-110`. |
| **No player logout handler** | Inventory never saved on leave | When client disconnects, gateway resets `client_ctrl_` (line 131) but never saves inventory state. |
| **PlayerInventory doesn't handle OnNetworkUpdate** | Server inventory pushes are ignored | `PlayerInventory` class inherits the empty default from `IUIWindow`. Incoming `InventoryUpdate` from gateway is delivered to `UIManager::HandleNetwork` → each window's `OnNetworkUpdate`, but `PlayerInventory` never updates `InventoryState`. |
| **Client drag-and-drop is local-only** | Inventory actions never sent to server | `PlayerInventory::Render` mutates `InventoryState` directly but never calls `NetClient::SendInventoryAction`. The server has zero visibility into client inventory changes. |
| **SendInventoryAction signature mismatch** | Cannot build valid `InventoryAction` | Current signature `(slot, item_id, count, stack_size, mb_id)` doesn't match FBS schema. Needs `(player_id, action_type, source_slot, target_slot, count)`. |
| **Gateway publishes inventory actions to `player.actions`** | MetaDB doesn't listen there | `kInventoryAction` goes through same `on_client_message` → `publish("player.actions")` handler as `kPlayerAction`. No service differentiates inventory-from-block actions. |
| **No MetaDB subscription to `player.inventory.actions`** | Inventory actions never reach MetaDB | MetaDB subscribes only to `meta_db.inventory.{get,set,snapshot}` (topic-based RPC) but not to any action-stream topic. |
| **InventoryAction has no `meta` field** | Item metadata lost on action | `InventoryAction` FBS table has `player_id, action_type, source_slot, target_slot, count` but no `meta:uint16`. Item variants/damage are silently dropped. |
| **No player_id tracking in client** | Client doesn't know its own ID | `GameClient::invState_` and `NetClient` have no player_id. `InventoryAction` requires `player_id:uint64`. |

### 1.3 Existing But Fragile

| Item | Issue |
|------|-------|
| **MetaDB JSON handler** (`db.go:297-411`) | Legacy JSON-over-TCP protocol (`:5005`). Has full `login/logout/update_slot` implementation but uses old JSON protocol. Keep for backward compat but primary path should be FlatBuffers. |
| **Client hardcoded test items** (`GameClient.cpp:104-110`) | `invState_.slots[0] = ItemStack{1, 1, 0};` etc. These initial items will be overwritten once inventory loading from MetaDB works. Must remove when login flow is implemented. |

---

## 2. Suggested Architecture

### 2.1 Data Flow Diagrams

#### Inventory Load (Player Login)

```
Client                     Gateway                         MessageRouter              MetaDB
  │                          │                                │                        │
  │  TCP connect (ctrl)      │                                │                        │
  │─────────────────────────>│                                │                        │
  │                          │   on_accept_ctrl_complete()    │                        │
  │                          │   (detect new client)          │                        │
  │                          │   publish("player.joined",     │                        │
  │                          │          {player_id, addr})    │                        │
  │                          │───────────────────────────────>│                        │
  │                          │                                │  dispatch to MetaDB    │
  │                          │                                │  subscriber             │
  │                          │                                │───────────────────────>│
  │                          │                                │                        │  m.GetInventory(playerID)
  │                          │                                │                        │───> SQLite
  │                          │                                │                        │<─── rows
  │                          │                                │  publish("player.      │
  │                          │                                │  inventory.update",    │
  │                          │                                │  InventoryUpdate{FB})  │
  │                          │                                │<───────────────────────│
  │                          │  router_read_cb:               │                        │
  │                          │  "player.inventory.update"     │                        │
  │                          │  → kInventoryUpdate            │                        │
  │                          │─────────────────────────────────────────────────────────>
  │  OnMessage(6, payload)   │                                │                        │
  │<─────────────────────────│                                │                        │
  │                          │                                │                        │
  │  onInventoryUpdate_      │                                │                        │
  │  → uiMgr_.HandleNetwork  │                                │                        │
  │  → PlayerInventory::     │                                │                        │
  │    OnNetworkUpdate       │                                │                        │
  │  → update InvState.slots │                                │                        │
```

#### Inventory Update (During Gameplay)

```
Client                     Gateway                         MessageRouter              MetaDB
  │                          │                                │                        │
  │  Player drags item       │                                │                        │
  │  → PlayerInventory       │                                │                        │
  │    mutates local state   │                                │                        │
  │  → calls SendInventory   │                                │                        │
  │    Action(action_type,   │                                │                        │
  │    source, target, count)│                                │                        │
  │                          │                                │                        │
  │  EnqueueWrite(7, FB)     │                                │                        │
  │─────────────────────────>│                                │                        │
  │                          │  client_ctrl_read_cb:          │                        │
  │                          │  kInventoryAction → verify     │                        │
  │                          │  → on_client_message           │                        │
  │                          │  → publish("player.            │                        │
  │                          │     inventory.actions", FB)    │                        │
  │                          │───────────────────────────────>│                        │
  │                          │                                │   route to MetaDB      │
  │                          │                                │   subscriber            │
  │                          │                                │───────────────────────>│
  │                          │                                │                        │  m.UpdateInventorySlot(
  │                          │                                │                        │    id, slot, item, count)
  │                          │                                │                        │───> SQLite
  │                          │                                │                        │
  │                          │                                │  publish("player.      │
  │                          │                                │  inventory.update",    │
  │                          │                                │  InventoryUpdate{FB})  │
  │                          │                                │<───────────────────────│
  │                          │  router_read_cb:               │                        │
  │                          │  "player.inventory.update"     │                        │
  │                          │  → kInventoryUpdate            │                        │
  │                          │─────────────────────────────────────────────────────────>
  │  OnMessage(6, payload)   │                                │                        │
  │<─────────────────────────│                                │                        │
  │                          │                                │                        │
  │  verify in OnNetwork     │                                │                        │
  │  Update (ack slot        │                                │                        │
  │  matches local state)    │                                │                        │
```

#### Inventory Save (Player Logout)

```
Client                     Gateway                         MessageRouter              MetaDB
  │                          │                                │                        │
  │  TCP disconnect          │                                │                        │
  │─────────────────────────>│                                │                        │
  │                          │  client_ctrl_->on_close        │                        │
  │                          │  → publish("player.left",      │                        │
  │                          │     {player_id})               │                        │
  │                          │───────────────────────────────>│                        │
  │                          │                                │  route to MetaDB       │
  │                          │                                │───────────────────────>│
  │                          │                                │                        │  (Inventory already saved
  │                          │                                │                        │   incrementally during
  │                          │                                │                        │   gameplay; optional:
  │                          │                                │                        │   save position)
  │                          │                                │                        │  m.SavePlayerPosition(id,
  │                          │                                │                        │    x, y, z)
```

### 2.2 Topic Map

| Topic | Publisher | Subscribers | Direction | Format |
|-------|-----------|-------------|-----------|--------|
| `player.joined` | Gateway (new) | MetaDB (new) | client→MetaDB | `{player_id:uint64}` |
| `player.left` | Gateway (new) | MetaDB (new) | client→MetaDB | `{player_id:uint64, x, y, z}` |
| `player.inventory.actions` | Gateway (new) | MetaDB (new) | client→MetaDB | `InventoryAction` |
| `player.inventory.update` | MetaDB | Gateway | MetaDB→client | `InventoryUpdate` |

### 2.3 Architecture Decisions

1. **Router-based, not direct TCP**: Gateway communicates with MetaDB through the MessageRouter pub/sub bus, not via direct TCP to MetaDB's `:5006` port. Rationale: avoids adding a second TCP connection from gateway; keeps all inter-service communication through the router for observability; MetaDB already has the `RouterClient` infrastructure.

2. **Incremental saves during gameplay**: Each inventory action is persisted to SQLite immediately as it happens (via `UpdateInventorySlot`). The logout flow only needs to save the player position, not a full inventory dump. Rationale: prevents data loss on crash; avoids large batch save on logout.

3. **Client is authoritative for local state, server validates**: Client mutates `InventoryState` immediately for UI responsiveness. The `InventoryAction` sent to server is the source of truth for persistence. Server publishes back `InventoryUpdate` as confirmation. If there's a conflict, server's version wins on next sync.

4. **One `player.inventory.actions` topic**: Inventory actions get their own topic (split from `player.actions`). This lets MetaDB subscribe without filtering, and allows SimulationCore to continue handling `player.actions` for block placement.

---

## 3. Components to Create/Modify

### Phase 1: Client-Side Inventory Action Sending

#### 3.1 Modify `NetClient.h` — Fix `SendInventoryAction` signature

**File**: `src/services/game_client/Network/NetClient.h:69-70`

Change:
```cpp
void SendInventoryAction(uint16_t slot, uint16_t item_id, uint8_t count,
                         uint32_t stack_size = 0, uint32_t mb_id = 0);
```
To:
```cpp
void SendInventoryAction(uint64_t player_id, uint8_t action_type,
                         uint8_t source_slot, uint8_t target_slot, uint8_t count);
```

#### 3.2 Modify `NetClient.cpp` — Implement proper `InventoryAction` FlatBuffer build

**File**: `src/services/game_client/Network/NetClient.cpp`

Replace the existing `SendInventoryAction` body with one that builds a `Protocol::InventoryAction` matching the FBS schema:
- `player_id:uint64`
- `action_type:uint8` (0=MOVE, 1=SPLIT, 2=DROP, 3=CRAFT)
- `source_slot:uint8`
- `target_slot:uint8` (0xFF = none/drop)
- `count:uint8`

#### 3.3 Modify `Common/Inventory.h` — Add `player_id` to `InventoryState`

**File**: `src/services/game_client/Common/Inventory.h`

Add field:
```cpp
uint64_t player_id = 0;
```

#### 3.4 Modify `PlayerInventory.cpp` — Send `InventoryAction` on drag-and-drop

**File**: `src/services/game_client/UI/Windows/player/PlayerInventory.cpp`

After each drag-and-drop operation (pickup, place, stack, swap), build and send an `InventoryAction` via `NetClient`:
- On pick-up: `MOVE, source_slot, 0xFF, count`
- On place: `MOVE, source_slot, target_slot, count`
- On swap: `MOVE, source_slot, target_slot, count` (full stack)
- On stack: `MOVE, source_slot, target_slot, partial_count`

The `PlayerInventory` class needs access to `NetClient`. Add a `NetClient*` member set via constructor or setter.

#### 3.5 Implement `PlayerInventory::OnNetworkUpdate` — Handle incoming inventory data

**File**: `src/services/game_client/UI/Windows/player/PlayerInventory.h/.cpp`

Override `OnNetworkUpdate(uint8_t msgType, const void* data)`:
- If `msgType == kInventoryUpdate`:
  - Parse `Protocol::InventoryUpdate` from data
  - Copy `slots` vector into `InventoryState::slots` (at appropriate offsets)
  - Mark any drag operation as cancelled if the server state conflicts

#### 3.6 Remove hardcoded test items

**File**: `src/services/game_client/GameClient.cpp:103-110`

Remove:
```cpp
invState_.slots[0]  = ItemStack{1, 1, 0};
invState_.slots[1]  = ItemStack{2, 16, 0};
// ... etc
```

### Phase 2: Gateway Routing for Inventory

#### 3.7 Modify `gateway.cpp` — Route `kInventoryAction` to `player.inventory.actions`

**File**: `src/services/game_client/gateway.cpp:378-383`

Change `client_ctrl_read_cb` to publish inventory actions to a dedicated topic instead of `player.actions`:
```cpp
case GatewayMsg::kInventoryAction: {
    flatbuffers::Verifier v(data, len);
    if (!v.VerifyBuffer<Protocol::InventoryAction>(nullptr)) {
        spdlog::error("Gateway: invalid InventoryAction on ctrl"); return;
    }
    publish("player.inventory.actions", data, len);  // was: on_client_message(data, len)
    break;
}
```

#### 3.8 Modify `gateway.cpp` — Add player connect/disconnect publishing

**File**: `src/services/gateway/gateway.cpp`

In `on_accept_ctrl_complete`, after client setup, publish `player.joined` to router:
```cpp
// At the end of on_accept_ctrl_complete:
if (router_ && router_registered_) {
    flatbuffers::FlatBufferBuilder fbb(64);
    auto msg = Protocol::CreatePlayerJoined(fbb, player_id);
    fbb.Finish(msg);
    publish("player.joined", fbb.GetBufferPointer(), fbb.GetSize());
}
```

In the `on_close` lambda (currently line 128-132), publish `player.left`:
```cpp
client_ctrl_->on_close = [this]() {
    spdlog::info("Gateway: ctrl client disconnected");
    if (router_ && router_registered_) {
        flatbuffers::FlatBufferBuilder fbb(64);
        auto msg = Protocol::CreatePlayerLeft(fbb, player_id);
        fbb.Finish(msg);
        publish("player.left", fbb.GetBufferPointer(), fbb.GetSize());
    }
    std::lock_guard<std::mutex> lock(client_ctrl_mutex_);
    client_ctrl_.reset();
};
```

This requires:
- Tracking `player_id` per connection (new field on `IoUringGateway`)
- Or having the first `PlayerAction` message identify the player, and caching the ID

#### 3.9 Modify `main.cpp` — Subscribe to new topics

**File**: `src/services/gateway/main.cpp`

Add subscriptions:
```cpp
gateway.subscribe("player.inventory.update");    // already exists (line 87)
gateway.subscribe("player.inventory.update");    // already subscribed
```

No new subscriptions needed on gateway side — `player.inventory.update` is already subscribed.

### Phase 3: MetaDB Subscriptions and Handlers

#### 3.10 Modify `router_client.go` — Subscribe to new topics

**File**: `src/services/meta_db/router_client.go:99-103`

Add topics:
```go
topics := []string{
    "meta_db.inventory.get",
    "meta_db.inventory.set",
    "meta_db.inventory.snapshot",
    "player.joined",
    "player.left",
    "player.inventory.actions",
}
```

#### 3.11 Add handler for `player.joined`

**File**: `src/services/meta_db/flatbuffer_tcp.go` (or new file `meta_db/player_events.go`)

Add a handler for the `player.joined` topic in `handlePublish`:
- Parse `player_id` from incoming message
- Call `m.GetInventory(playerID)` to load from SQLite
- Call `m.PublishInventoryUpdate(playerID, slots)` to push to gateway→client

This is the **inventory load on join** flow.

#### 3.12 Add handler for `player.left`

**File**: `src/services/meta_db/flatbuffer_tcp.go` (or new file)

Add a handler for `player.left`:
- Parse `player_id, x, y, z`
- Call `m.SavePlayerPosition(playerID, x, y, z)`

(Inventory is already incrementally saved during gameplay.)

#### 3.13 Add handler for `player.inventory.actions`

**File**: `src/services/meta_db/flatbuffer_tcp.go` (or new file)

Add a handler for `player.inventory.actions`:
- Parse `Protocol::InventoryAction`
- Switch on `action_type`:
  - `MOVE(0)` / `SPLIT(1)`: Call `m.UpdateInventorySlot(playerID, target_slot, item_id, count)` for the target. If source becomes empty, also handle that.
  - `DROP(2)`: Call `m.UpdateInventorySlot(playerID, source_slot, 0, 0)` (clear slot).
  - `CRAFT(3)`: Full inventory replacement via `m.CreateInventory(playerID, newSlots)`.
- After update, publish `player.inventory.update` event via `m.PublishInventoryUpdate`

### Phase 4: Protocol Updates

#### 3.14 Add `player_id` field to `gateway.h` connection tracking

**File**: `src/services/gateway/gateway.h`

Add member:
```cpp
uint64_t client_player_id_ = 0;
```

Add method to set it when first `PlayerAction` arrives (the action already carries `player_id`):
```cpp
void set_client_player_id(uint64_t id) { client_player_id_ = id; }
```

#### 3.15 Add `PlayerJoined`/`PlayerLeft` tables to `core.fbs`

**File**: `src/protocol/core.fbs`

Add:
```
table PlayerJoined {
  player_id:uint64;
}

table PlayerLeft {
  player_id:uint64;
  x:int32;
  y:int32;
  z:int32;
}
```

#### 3.16 Add `meta` field to `InventoryAction` (optional but recommended)

**File**: `src/protocol/core.fbs:167-173`

Add `meta:uint16` to `InventoryAction`:
```
table InventoryAction {
  player_id:uint64;
  action_type:uint8;
  source_slot:uint8;
  target_slot:uint8;
  count:uint8;
  meta:uint16;          // ← ADD: item metadata/damage
}
```

This is a non-breaking addition (optional field defaults to 0).

---

## 4. Order of Implementation

```
Phase 1: Client
  └── 3.3 Inventory.h: Add player_id field
  └── 3.1 NetClient.h: Fix SendInventoryAction signature
  └── 3.2 NetClient.cpp: Build proper InventoryAction FB
  └── 3.4 PlayerInventory.cpp: Send actions on drag-and-drop
  └── 3.5 PlayerInventory: Implement OnNetworkUpdate
  └── 3.6 GameClient.cpp: Remove hardcoded test items

Phase 2: Gateway
  └── 3.14 gateway.h: Add client_player_id_ tracking
  └── 3.7 gateway.cpp: Route kInventoryAction to player.inventory.actions
  └── 3.8 gateway.cpp: Publish player.joined/player.left
  └── 3.9 main.cpp: (already subscribed, no changes needed)

Phase 3: MetaDB
  └── 3.10 router_client.go: Subscribe to new topics
  └── 3.11 flatbuffer_tcp.go: Handle player.joined → load inventory
  └── 3.13 flatbuffer_tcp.go: Handle player.inventory.actions → save + publish
  └── 3.12 flatbuffer_tcp.go: Handle player.left → save position

Phase 4: Protocol
  └── 3.15 core.fbs: Add PlayerJoined/PlayerLeft tables
  └── 3.16 core.fbs: Add meta to InventoryAction
  └── Regenerate FlatBuffers: make generate-fbs

Phase 5: Testing
  └── client: Unit test for SendInventoryAction build
  └── MetaDB: Update e2e_test.go with player.joined/player.left flow
  └── Integration: Start all services, connect client, verify:
      - Login loads inventory from SQLite
      - Drag-and-drop saves to SQLite
      - Logout saves position
```

**Dependencies**:
- Phase 1 depends on: nothing (can be developed/tested with mock gateway)
- Phase 2 depends on: Phase 1 (client must send proper actions)
- Phase 3 depends on: Phase 4 (protocol types for PlayerJoined/PlayerLeft)
- Phase 4 can happen in parallel with Phase 1-2
- Phase 5 depends on: all phases

---

## 5. Risks and Open Questions

### 5.1 Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| **Inventory state conflicts**: Client modifies local state immediately, but server rejects (e.g., item doesn't exist in source slot). | Medium | Medium | Client should revert on mismatch between sent action and received `InventoryUpdate`. Track pending actions with a sequence number. |
| **Race on login**: Client sends `PlayerAction` (block placement) before inventory loads. Client might try to use an item it doesn't have yet. | Low | Low | Gateway should queue outbound messages until inventory is loaded and sent. Or client simply waits for first `InventoryUpdate` before allowing block placement. |
| **MetaDB RouterClient single connection**: `handlePublish` processes all topics on one goroutine. Heavy `player.inventory.actions` traffic could block `meta_db.inventory.get` RPC calls. | Medium | Medium | Use separate goroutines per topic in MetaDB's dispatch, or add async writes to SQLite. |
| **`InventoryAction` lacks `meta` field**: Item metadata (e.g., damage, color) will be 0 after any inventory move, destroying stateful item data. | High | High | Add `meta:uint16` to `InventoryAction` before implementation starts. |
| **Client doesn't know `player_id`**: The auth system isn't built yet. On first connect, the server assigns a `player_id` and sends it to the client. | High | Blocking | Need a handshake: gateway assigns temp ID on connect, or client includes ID in first `PlayerAction`. For MVP, use a hardcoded player_id (e.g., 1) and pass it in `GameClient::Init`. |

### 5.2 Open Questions

1. **When does the player_id get assigned?** Is it from a login/auth step, or do we use a hardcoded dev ID for now? The entire chain depends on knowing the player_id at connect time.

2. **Inventory size and slot layout?** `PlayerInventory` uses 40 slots (10 hotbar + 30 inventory). `InventoryUpdate.slots` is a flat array. What's the mapping from slot index to screen position? (Currently: 0-9 hotbar, 10-39 inventory grid.)

3. **Should the gateway use direct TCP to MetaDB (`:5006`) instead of router pub/sub?** The FlatBuffer TCP port `:5006` exists and works (tested in `e2e_test.go`). Router-based is simpler to maintain but adds latency. Direct TCP is lower latency but requires another connection from gateway. **Recommendation**: Use router for consistency; switch to direct TCP if latency becomes an issue.

4. **What happens if MetaDB is down?** Client can still play with local inventory, but no persistence. Should the gateway buffer inventory actions and replay when MetaDB reconnects? **Recommendation**: For MVP, log and drop. Add buffering in a later iteration.

5. **Should `InventoryUpdate` carry the full inventory or just deltas?** The FBS schema currently defines it as full-slot-array. For large inventories this is wasteful. **Recommendation**: Keep full-sync for MVP (simplicity). Add delta encoding when inventory exceeds 40 slots (e.g., chests with 54+ slots).

6. **Does `IPlayerInventoryStorage` need a concrete `MetaDBTcpClient` class?** Or can the gateway go entirely through the router without implementing this interface? With the router-based approach, `InventoryIntegration` and `IPlayerInventoryStorage` are bypassed. They're only needed if we use direct TCP. **Recommendation**: For router-based architecture, `IPlayerInventoryStorage` is unnecessary. Remove it or repurpose as a pure in-memory cache layer. Gateway routes messages, it doesn't store state.

### 5.3 Decisions Made

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Communication path | Router pub/sub (not direct TCP) | Consistency; reuses existing MetaDB router client; observability |
| Save strategy | Incremental during gameplay | Prevents data loss on crash; no batch save on logout |
| Client authority | Optimistic local update + server confirmation | UI responsiveness; server version wins on conflict |
| Gateway role | Thin router (no storage logic) | Keeps gateway focused on connection management; storage logic belongs in MetaDB |

---

## Appendix: File Change Summary

| # | File | Change Type | Description |
|---|------|-------------|-------------|
| 1 | `protocol/core.fbs` | Modify | Add `PlayerJoined`, `PlayerLeft` tables; add `meta` to `InventoryAction` |
| 2 | `protocol/gateway.fbs` | No change | Types already defined |
| 3 | `protocol/meta_db.fbs` | No change | RPC types already complete |
| 4 | `game_client/Common/Inventory.h` | Modify | Add `player_id` to `InventoryState` |
| 5 | `game_client/Network/NetClient.h` | Modify | Fix `SendInventoryAction` signature; add `player_id` param |
| 6 | `game_client/Network/NetClient.cpp` | Modify | Implement proper `InventoryAction` FB build |
| 7 | `game_client/UI/Windows/player/PlayerInventory.h` | Modify | Add `NetClient*` member; override `OnNetworkUpdate` |
| 8 | `game_client/UI/Windows/player/PlayerInventory.cpp` | Modify | Send `InventoryAction` on drag-and-drop; handle `OnNetworkUpdate` |
| 9 | `game_client/GameClient.cpp` | Modify | Set `invState_.player_id`; remove hardcoded test items |
| 10 | `gateway/gateway.h` | Modify | Add `client_player_id_` field and setter |
| 11 | `gateway/gateway.cpp` | Modify | Route `kInventoryAction` to `player.inventory.actions`; publish `player.joined`/`player.left` |
| 12 | `meta_db/router_client.go` | Modify | Subscribe to `player.joined`, `player.left`, `player.inventory.actions` |
| 13 | `meta_db/flatbuffer_tcp.go` | Modify | Add dispatch for new action/inventory topics |
| 14 | `meta_db/db.go` | No change | SQLite functions already complete |
| 15 | `meta_db/e2e_test.go` | Modify | Add tests for player join/leave + action flow |
