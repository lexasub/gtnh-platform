{
  "title": "Примитивные типы (Primitive Types)",
  "description": "Define primitive types: ItemStack (uint16 item_id + uint8 count + uint16 meta), Liquid (uint32 fluid_id + int32 amount), MachineType (uint8 enum). All numeric for zero-copy wire format. Item names/display data resolved via ItemRegistry on demand.",
  "ecs_components": [],
  "flatbuffers_schemas": [
    {
      "name": "ItemStack",
      "fields": [
        {"name": "item_id", "type": "uint16"},
        {"name": "count", "type": "uint8"},
        {"name": "meta", "type": "uint16"}
      ]
    },
    {
      "name": "Liquid",
      "fields": [
        {"name": "fluid_id", "type": "uint32"},
        {"name": "amount", "type": "int32"}
      ]
    },
    {
      "name": "MachineType",
      "fields": [
        {"name": "machine_type", "type": "uint8"}
      ]
    }
  ],
  "service_architecture": "Shared data layer used by all services. ItemStack, Liquid (FluidStack/FluidTank), and MachineType are defined in the FlatBuffers schema (core.fbs, recipe.fbs) and compiled to C++/Go stubs for zero-copy access. ItemRegistry provides human-readable name/display data on demand only (lazy).",
  "inputs": {},
  "constraints": [
    "ItemStack.item_id = uint16 (0–65535). No string IDs on wire. String names only in CSV→SQLite ItemRegistry for display.",
    "ItemStack.count = uint8 (0–255, practical max stack = 127, 0 = empty slot)",
    "ItemStack.meta = uint16 (damage/state, 0 = default/new, values < 32768 are 'normal', >= 32768 = special)",
    "Liquid.fluid_id = uint32 (0 = empty). String names only for display via registry.",
    "MachineType = uint8 enum (0=NONE, 1=FURNACE, 2=ASSEMBLER, 3=CRYSTALLIZER, 4=ELECTROLYSER, 5=CHEMICAL_REACTOR)",
    "All types must be FlatBuffers-compatible for zero-copy transmission",
    "ItemRegistry: SQLite table items(id INTEGER PK, name TEXT, stack_size INT, meta INT). CSV source → SQLite build artifact."
  ],
  "test_requirements": "Verify ItemStack, Liquid, and MachineType serialize/deserialize correctly via FlatBuffers. Verify uint16 item_id range is enforced. Verify zero-copy access patterns work as expected. Verify ItemRegistry returns correct name for uint16 item_id."
}
