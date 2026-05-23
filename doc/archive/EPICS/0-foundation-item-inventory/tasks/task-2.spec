{
  "title": "Inventory — контейнер ItemStack'ов",
  "description": "Generic container with a fixed number of slots (dynamic sizing deferred to post-MVP). Each slot contains an ItemStack or is empty (item_id = 0). Provides operations for add, remove, move, and swap items between slots.",
  "ecs_components": ["Inventory"],
  "flatbuffers_schemas": [
    {
      "name": "InventorySlot",
      "fields": [
        {"name": "item_id", "type": "uint16"},
        {"name": "count", "type": "uint8"},
        {"name": "meta", "type": "uint16"}
      ]
    },
    {
      "name": "Inventory",
      "fields": [
        {"name": "slots", "type": "[InventorySlot]"},
        {"name": "size", "type": "uint8"}
      ]
    }
  ],
  "service_architecture": "Inventory is a server-side concept. SimulationCore hosts Inventory components in ECS. Clients send actions, server mutates inventory state, and broadcasts InventoryUpdate messages. No client-side authoritative state.",
  "inputs": {
    "InventoryAction": {
      "inputs": ["player_id", "action_type", "source_slot", "target_slot", "count"],
      "outputs": ["player_id", "updated_slots"]
    }
  },
  "constraints": [
    "Fixed slot count per inventory type (no dynamic resizing on MVP)",
    "Slots indexed 0..N-1",
    "item_id = 0 means empty slot",
    "Count never exceeds stack limit per item_id"
  ],
  "test_requirements": "Test slot operations: add item to empty slot, add to partially filled slot, remove from slot, swap between slots, overflow handling when slot at max count."
}
