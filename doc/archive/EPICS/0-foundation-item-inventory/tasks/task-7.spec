{
  "title": "Protocol — FlatBuffers схемы инвентарей",
  "description": "FlatBuffers protocol messages for inventory operations. Defines InventorySlot, InventoryUpdate, InventoryAction, and related messages in protocol/core.fbs. Shared across all services that handle items. Zero-copy parsing for performance.",
  "ecs_components": [],
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
      "name": "InventoryUpdate",
      "fields": [
        {"name": "player_id", "type": "uint64"},
        {"name": "slots", "type": "[InventorySlot]"}
      ]
    },
    {
      "name": "InventoryAction",
      "fields": [
        {"name": "player_id", "type": "uint64"},
        {"name": "action_type", "type": "uint8"},
        {"name": "source_slot", "type": "uint8"},
        {"name": "target_slot", "type": "uint8"},
        {"name": "count", "type": "uint8"}
      ]
    }
  ],
  "service_architecture": "All inventory protocol messages defined in protocol/core.fbs under the Protocol namespace. FlatBuffers compiler (flatc) generates C++ and Go bindings. Messages routed via MessageRouter (Go) on pub/sub topics by message type.",
  "inputs": {},
  "outputs": {},
  "constraints": [
    "All inventory messages must be in Protocol namespace",
    "InventoryUpdate is a full snapshot (not a delta) on MVP",
    "InventorySlot reuses ItemStack layout (item_id+count+meta)",
    "Zero-copy parsing required in C++ services",
    "Array fields use FlatBuffers [T] notation"
  ],
  "test_requirements": "Verify FlatBuffers schema compiles with flatc. Test serialization round-trip: build InventoryUpdate → serialize → parse → verify fields match. Benchmark zero-copy access vs generated accessors."
}
