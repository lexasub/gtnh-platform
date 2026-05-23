{
  "title": "Открытые вопросы (Open Questions)",
  "description": "Design decisions deferred for resolution during implementation: blob serialization format (FlatBuffers vs binary), chest inventory storage location (EntityStateStore vs ChunkStore), stack size limits (vanilla 64 vs GTNH custom), and NBT tag container预留 for future expansion.",
  "ecs_components": [],
  "flatbuffers_schemas": [],
  "service_architecture": "Open questions affecting multiple services. Resolution required before or during implementation of task-4 (storage architecture) and task-5 (EntityStateStore). Each decision may ripple to protocol schema, storage backend choice, and client rendering.",
  "inputs": {},
  "constraints": [
    "Q1: Blob format — FlatBuffers vs simple binary dump for MetaDB/EntityStateStore storage",
    "Q2: Chest inventory — in EntityStateStore or in ChunkStore as part of block data?",
    "Q3: Stack limits — standard Minecraft 64 or GTNH-custom stack sizes per item?",
    "Q4: NBT future — reserve tag container in ItemStack for future extensions?"
  ],
  "test_requirements": "Document decisions with rationale. Each resolved question should update relevant task specs and protocol schemas accordingly."
}
