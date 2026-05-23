# Task 1: Player & Inventories

## Affected Services

| Service | Role | R/W |
|---------|------|-----|
| **MetaDB** (Go/SQLite) | Primary — owns player inventory CRUD, stores position | Read/Write |
| **SimulationCore** | Post-hook — subscribes to inventory events for gameplay (crafting completion, item consumption rules) | Event subscriber |
| **Gateway** | Relay — forwards PlayerAction, InventoryUpdate between client and services | Relay |
| **GameClient** | Consumer — inventory GUI, hotbar, creative menu, crafting UI | Read/Write |
| **RecipeManager** ⬅️ NEW | Dependency — validates craftable recipes on CraftRequest | Read |

## Task Title
Player & Inventories Integration

## Description
This task involves implementing player inventory management and interaction systems, including item placement, retrieval, and usage mechanics.

## ECS Components
- Client: Handles inventory UI and local state
- MetaDB: Stores player inventories persistently
- SimulationCore: Validates item interactions and resource consumption
- RecipeManager: Processes crafting recipes and resource checks

## FlatBuffers Schemas
```flatbuffers
table PlayerAction {
    player_id: uint64;
    action: PlayerActionType;  // PLACE, BREAK, MOVE, USE
    x: uint32; y: uint32; z: uint32;
    block_id: uint16;
    selected_slot: uint8;      // ← new field for block selection
}

table InventoryUpdate {
    player_id: uint64;
    slots: [ItemStack];
}
```

## Service Architecture
- **Client**: Exposes inventory UI and sends actions to server
- **Gateway**: Routes inventory requests between client and service
- **MetaDB**: Provides read access to player inventories
- **SimulationCore**: Validates item operations and resource management
- **RecipeManager**: Central service for recipe lookups and crafting logic

## Inputs/Outputs
- **Inputs**: Player position (x,y,z), block ID, item slot index, action type
- **Outputs**: Updated inventory state, resource amounts, success/failure status

## Constraints
- No falling items without explicit lure
- No tool dependency for item breakdown (items don\'t consume resources)
- No block strength checks (MVP)
- Trust client for all actions (server verifies on receipt)

## Test Requirements
- Unit tests for item placement/breakage logic
- Integration tests for inventory persistence
- Performance tests for ECS with 10k+ items
- Network latency tests for action synchronization
