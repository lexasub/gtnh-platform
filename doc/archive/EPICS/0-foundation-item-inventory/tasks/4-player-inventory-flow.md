# TASK: Player Inventory Flow
**Layer**: 1
**Status**: Draft
**Epic**: 0-foundation-item-inventory

## Affected Services

| Service | Role | R/W |
|---------|------|-----|
| **MetaDB** (Go/SQLite) | Primary — owns player inventory CRUD, persists to SQLite | Read/Write |
| **Gateway** | Relay — fetches inventory on login, pushes InventoryUpdate to client | Relay |
| **SimulationCore** | Post-hook — subscribes to inventory events for gameplay effects (crafting, item consumption) | Event subscriber |
| **GameClient** | Consumer — displays inventory GUI, sends slot interactions | Read/Write |

> **Architecture rule**: Inventory is owned by MetaDB. SimulationCore does NOT manage inventory state — it only hooks into inventory events (post-hooks) for gameplay. The flow: Client ↔ Gateway ↔ MetaDB (inventory CRUD), and MetaDB publishes events → SimulationCore reacts.

# Overview

The Player Inventory Flow manages the lifecycle of a player's inventory data from MetaDB through the Gateway to the GameClient. The inventory consists of 36 main slots plus 9 hotbar slots for a total of 45 slots. Each slot tracks an item ID, count, and durability.

The inventory persists in MetaDB (Go, SQLite) and synchronizes over the binary protocol via the InventoryUpdate FlatBuffer message. The Gateway acts as the intermediary, fetching inventory on login and relaying changes back to MetaDB on logout.

# Login Flow

When a player connects, the Gateway retrieves their inventory from MetaDB and synchronizes it to the client:

1. Gateway sends `GetInventory(player_id)` RPC to MetaDB
2. MetaDB returns serialized inventory blob (45 slots × 12 bytes each)
3. Gateway deserializes into `InventoryUpdate` FlatBuffer message
4. Gateway sends `InventoryUpdate` to GameClient
5. GameClient populates the UI and internal slot state

# Runtime Flow

Inventory modifications (slot clicks, crafting, etc.) follow this path:

1. GameClient emits `InventoryUpdate` with changed slot(s)
2. Gateway receives and validates the change
3. Gateway sends `SetInventory(slot_id, item_id, count, durability)` to MetaDB
4. MetaDB writes atomically to player table
5. Gateway acknowledges `InventoryUpdateAck` to GameClient

# Logout Flow

On logout, the inventory must be persisted to disk before the connection closes:

1. GameClient emits `InventorySave` event
2. Gateway sends `SaveInventory(player_id)` to MetaDB
3. MetaDB serializes full inventory and writes to player table
4. MetaDB calls `db.conn.Commit()` to flush to disk
5. Gateway sends `InventorySaved` to GameClient
6. Gateway closes TCP connection

# InventoryUpdate Protocol Usage

The `InventoryUpdate` FlatBuffer message carries slot modifications:

```flatbuffers
struct SlotUpdate {
  uint8_t slot_id;     // 0–44 (36 main + 9 hotbar)
  uint16_t item_id;    // 0 = air
  uint8_t  count;      // 0–64
  uint8_t  durability; // 0–255
};

struct InventoryUpdate {
  uint64_t player_id;
  SlotUpdate slots[45];
};
```

Changes are delta-encoded: only modified slots are included in the array. The Gateway deduplicates consecutive updates to reduce network chatter.

# File Locations

| File | Path |
|------|------|
| MetaDB implementation | `src/services/meta_db/inv.go` |
| Gateway inventory handler | `src/services/gateway/inv.go` |
| GameClient inventory UI | `src/services/game_client/gui/inv_ui.h` |
| Protocol schema | `src/protocol/inv.fbs` |
| FlatBuffers generated | `src/protocol/inv_generated.h` |

# Acceptance Criteria

#### Scenario: Login load

1. Player connects to Gateway
2. Gateway sends `GetInventory(player_id)` to MetaDB
3. MetaDB returns serialized blob
4. Gateway sends `InventoryUpdate` to GameClient
5. GameClient displays 45 slots with correct items

#### Scenario: Runtime update

1. Player clicks slot 5 to craft a tool
2. GameClient emits `InventoryUpdate` with slot 5 changed
3. Gateway sends `SetInventory(5, new_item_id, 1, 100)` to MetaDB
4. MetaDB writes to player table
5. Gateway sends `InventoryUpdateAck` to GameClient

#### Scenario: Logout save

1. Player logs out
2. GameClient emits `InventorySave`
3. Gateway sends `SaveInventory(player_id)` to MetaDB
4. MetaDB serializes all 45 slots and writes to disk
5. MetaDB calls `Commit()` to flush
6. Gateway sends `InventorySaved` to GameClient
7. Gateway closes TCP connection
