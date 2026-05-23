{
  "title": "Открытые вопросы (Open Questions)",
  "description": "Document unresolved design questions. Q1 (JSON format) and Q2 (binary format) resolved: compact JSON for storage, uint16 on wire. Remaining: temperature/liquid/purity/biome data sources, ItemRegistry sync/versioning, NBT serialization format for output overrides.",
  "ecs_components": [],
  "flatbuffers_schemas": [],
  "service_architecture": "Resolved: Q1 — per-machine JSON files (furnace.json, assembler.json). Q2 — not FlatBuffers for storage; compact JSON + uint16 on wire is sufficient. Remaining questions affect ItemRegistry, NBT handling, and Layer 2 data flow.",
  "inputs": {},
  "constraints": [
    "Q1: RESOLVED — per-machine JSON files (furnace.json, assembler.json, etc.) with compact format",
    "Q2: RESOLVED — compact JSON for storage, uint16 on wire (FlatBuffers). No binary format needed",
    "Q3: Temperature source — how does RecipeManager obtain machine temperature? (RPC from Machine?)",
    "Q4: Liquid volume — how does RecipeManager determine liquid volume in machine tank?",
    "Q5: Purity — how does RecipeManager measure/obtain current machine purity?",
    "Q6: Biome — how does RecipeManager query the biome at machine coordinates? (SpatialIndex?)",
    "Q7: Special conditions — how to implement custom condition logic (player proximity, time of day)?",
    "Q8: Category extension — how to add new recipe categories without breaking changes?",
    "Q9: NEW — ItemRegistry sync: how does client get fresh items.db? Embedded in binary? HTTP from MetaDB?",
    "Q10: NEW — ItemRegistry versioning: what if item_id changes between modpack versions? SQL migration?",
    "Q11: NEW — NBT format in output overrides: flat JSON → NBT binary, or full NBT spec? Protobuf/FlatBuffers for structured NBT?"
  ],
  "test_requirements": "No test requirements until open questions are resolved. These are design discussion points for the team."
}
