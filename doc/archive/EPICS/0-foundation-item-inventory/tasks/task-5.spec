{
  "title": "EntityStateStore — сервис хранения TileEntity",
  "description": "Planned C++ service for persisting TileEntity/machine states independently from ChunkStore. Backend: LMDB. Key: dim|x|y|z → blob. RPC: GetState(key) → blob, SetState(key, blob). Designed for frequent writes (machine ticking) without affecting chunk read performance.",
  "ecs_components": ["EntityStateStore"],
  "flatbuffers_schemas": [
    {
      "name": "EntityStateRequest",
      "fields": [
        {"name": "key", "type": "string"},
        {"name": "operation", "type": "uint8"}
      ]
    },
    {
      "name": "EntityStateResponse",
      "fields": [
        {"name": "key", "type": "string"},
        {"name": "blob", "type": "[uint8]"},
        {"name": "exists", "type": "bool"}
      ]
    }
  ],
  "service_architecture": "Standalone C++ service with LMDB backend. Receives GetState/SetState RPCs via MessageRouter. SimulationCore and Gateway are primary clients. No chunk awareness — purely key-value for tile entity state blobs. Blob format is opaque to the store (dumb storage principle).",
  "inputs": {
    "SetState": {
      "inputs": ["key", "blob"],
      "outputs": ["success"]
    },
    "GetState": {
      "inputs": ["key"],
      "outputs": ["blob", "exists"]
    }
  },
  "constraints": [
    "Key format: dim|x|y|z (uint64 packed or string, TBD)",
    "Blob is opaque binary data (no schema enforcement at store level)",
    "LMDB read-optimized for frequent GetState calls",
    "Separate process from ChunkStore (independent scaling)",
    "No caching layer on MVP (direct LMDB reads)"
  ],
  "test_requirements": "Unit tests for SetState/GetState round-trip. Test LMDB persistence: write state, restart service, read back. Test concurrent reads/writes. Test key collision isolation between different dimensions."
}
