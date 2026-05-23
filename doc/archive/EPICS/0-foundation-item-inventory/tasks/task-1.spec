{
  "title": "ItemStack — базовый тип данных",
  "description": "Core data type for all item operations across services. Abstract ItemStack = item_id + count + meta (durability/state/damage). No NBT on MVP. Used by inventories, recipes, drops, and machines as the universal item representation.",
  "ecs_components": [],
  "flatbuffers_schemas": [
    {
      "name": "ItemStack",
      "fields": [
        {"name": "item_id", "type": "uint16"},
        {"name": "count", "type": "uint8"},
        {"name": "meta", "type": "uint16"}
      ]
    }
  ],
  "service_architecture": "Shared type across all services. Defined in protocol/core.fbs. Consumed by Gateway, SimulationCore, ChunkStore, MetaDB, and EntityStateStore for any item-related operation.",
  "inputs": {},
  "outputs": {},
  "constraints": [
    "item_id must be uint16 (1–65535, 0 = empty/invalid)",
    "count must be uint8 (1–64 MVP; configurable per item later)",
    "meta must be uint16 (0 = new/pristine; 65535 = max damage)",
    "No negative counts or metas",
    "No NBT on MVP (reserved for future extension)"
  ],
  "test_requirements": "Unit tests for serialization/deserialization across FlatBuffers. Boundary tests: count at 1 and 64, meta at 0 and 65535. Ensure zero item_id represents empty slot."
}
