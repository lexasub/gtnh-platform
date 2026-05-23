{
  "title": "Типы инвентарей (Player, Machine, Workbench, Chest)",
  "description": "Different inventory types with varying slot counts and persistence backends. Player: 36+9 hotbar. Machine: input/output (machine-dependent). Workbench: 3x3 + result. Chest: 9xN (post-MVP). Each type maps to the appropriate storage service.",
  "ecs_components": ["PlayerInventory", "MachineInventory", "CraftingTableInventory", "ChestInventory"],
  "flatbuffers_schemas": [
    {
      "name": "InventoryType",
      "fields": [
        {"name": "type", "type": "uint8"},
        {"name": "slot_count", "type": "uint16"}
      ]
    }
  ],
  "service_architecture": "PlayerInventory stored in MetaDB as serialized blob. Machine/Workbench/Chest inventories stored in EntityStateStore keyed by dimension+coordinates. Gateway orchestrates loading: on login loads from MetaDB, on chunk load loads from EntityStateStore.",
  "inputs": {
    "InventoryTypeQuery": {
      "inputs": ["player_id", "x", "y", "z", "inventory_type"],
      "outputs": ["slot_count", "storage_backend"]
    }
  },
  "constraints": [
    "Player: 36 main + 9 hotbar = 45 slots",
    "Machine: input/output count depends on machine definition",
    "Workbench: 9 crafting slots + 1 result = 10 slots",
    "Chest: deferred to post-MVP (9xN dynamic)",
    "Each type maps to exactly one storage backend"
  ],
  "test_requirements": "Verify each inventory type creates with correct slot count. Test that player inventory persists across login/logout cycles. Test machine inventory persists across chunk load/unload cycles."
}
