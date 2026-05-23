{
  "title": "Источник ресурсов (Creative Menu)",
  "description": "Player needs a way to obtain items for testing crafts and machines without tools or resources.",
  "ecs_components": ["CreativeMenu", "InventoryUpdate"],
  "flatbuffers_schemas": [],
  "service_architecture": "CreativeMenu UI service receives item selection from player and updates inventory via InventoryUpdate messages.",
  "inputs": {
    "CreativeMenu": {
      "inputs": ["item_id", "quantity", "action"],
      "outputs": ["updated_inventory"]
    }
  },
  "constraints": [],
  "test_requirements": "Verify menu allows testing all mechanics and that resource drops are correctly applied to player inventory."
}