{
  "title": "PlayerInventory — жизненный цикл игрока",
  "description": "Player inventory lifecycle: on login, Gateway requests serialized inventory from MetaDB → sends InventoryUpdate to client. Client maintains local copy and renders in ImGui. On logout / periodic save, client sends inventory state back via Gateway → MetaDB persists.",
  "ecs_components": ["PlayerInventory", "MetaDBInventory"],
  "flatbuffers_schemas": [
    {
      "name": "InventoryUpdate",
      "fields": [
        {"name": "player_id", "type": "uint64"},
        {"name": "slots", "type": "[InventorySlot]"}
      ]
    }
  ],
  "service_architecture": "Flow:\n1. Client connects → Gateway authenticates → requests PlayerInventory from MetaDB\n2. MetaDB returns serialized InventoryUpdate blob\n3. Gateway forwards to client via InventoryUpdate message\n4. Client renders inventory in ImGui\n5. On player action (move/use/drop item) → Client sends PlayerAction → SimulationCore validates → new InventoryUpdate broadcast\n6. On disconnect/save-interval → MetaDB persists current state\n\nMetaDB stores as serialized blob (FlatBuffers or binary, TBD).",
  "inputs": {
    "LoadInventory": {
      "inputs": ["player_id"],
      "outputs": ["InventoryUpdate"]
    },
    "SaveInventory": {
      "inputs": ["player_id", "slots"],
      "outputs": ["success"]
    }
  },
  "constraints": [
    "Inventory loaded on login before player can interact with world",
    "Client has local copy but is NOT authoritative (server validates)",
    "MetaDB is the source of truth for player inventory",
    "Save on logout: loss of unsaved changes on crash acceptable on MVP",
    "Periodic autosave interval: TBD (30s? 60s?)"
  ],
  "test_requirements": "Integration test: login → load inventory → verify slots. Modify inventory → logout → login → verify state persisted. Stress test: 100 concurrent players logging in/out with inventory operations."
}
