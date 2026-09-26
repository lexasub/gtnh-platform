// Shared declaration between test_chunk_store_repository_stub.cpp and
// stub_chunk_client.cpp. See stub_chunk_client.cpp for why the stub exists.
#pragma once

#include <cstdint>
#include <vector>

namespace simcore {

// Everything the stub observed, plus the scripted response it hands back.
// Reset it with resetStub() so one test's traffic cannot leak into the next.
struct StubChunkClientControl {
  bool connected = false;

  struct CasCall {
    int32_t x = 0, y = 0, z = 0;
    uint16_t expected_block_id = 0;
    uint16_t new_block_id = 0;
    uint8_t meta = 0;
  };
  struct GetCall {
    int32_t x = 0, y = 0, z = 0;
  };

  std::vector<CasCall> cas_calls;
  std::vector<GetCall> get_calls;

  // Scripted connected-mode reply.
  uint8_t reply_status = 0;
  uint16_t reply_block_id = 0;
  uint8_t reply_meta = 0;
  uint32_t reply_mb_id = 0;

  void reset() {
    connected = false;
    cas_calls.clear();
    get_calls.clear();
    reply_status = 0;
    reply_block_id = 0;
    reply_meta = 0;
    reply_mb_id = 0;
  }
};

extern StubChunkClientControl g_stub;

} // namespace simcore
