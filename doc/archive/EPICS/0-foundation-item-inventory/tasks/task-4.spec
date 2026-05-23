{
  "title": "Архитектура хранения (MetaDB / EntityStateStore / ChunkStore)",
  "description": "Clear storage separation rule: MetaDB for player-bound data (inventory, equipment), EntityStateStore for world-bound data (machine/chest/workbench state), ChunkStore never stores inventories — only blocks and meta-layer. Prevents ChunkStore from becoming write-heavy.",
  "ecs_components": [],
  "flatbuffers_schemas": [],
  "service_architecture": "Three-tier storage architecture:\n1. MetaDB (Go + SQLite): Player inventory blobs, keyed by player_id. Loaded on login, saved on logout.\n2. EntityStateStore (C++ + LMDB): TileEntity state blobs, keyed by dim|x|y|z. Accessed by SimulationCore on block interaction.\n3. ChunkStore (C++ + LMDB): Block IDs and meta-layer only. Never reads/writes inventory data.\n\nGateway routes inventory requests: if player-bound → MetaDB, if world-bound → EntityStateStore.",
  "inputs": {
    "InventoryLoadRequest": {
      "inputs": ["player_id", "dimension", "x", "y", "z"],
      "outputs": ["storage_backend", "blob"]
    }
  },
  "constraints": [
    "ChunkStore must NEVER store inventory/TileEntity data",
    "MetaDB only for player-bound data (not world-bound)",
    "EntityStateStore only for world-bound data (not player-bound)",
    "EntityStateStore key format: dim|x|y|z (string or packed uint64)",
    "No cross-service references in stored blobs (dumb storage)"
  ],
  "test_requirements": "Verify player inventory loads from MetaDB on login. Verify machine inventory loads from EntityStateStore on chunk interaction. Verify ChunkStore rejects inventory write attempts."
}
