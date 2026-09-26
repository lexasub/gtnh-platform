// Shared declaration between WorldContainerInventory_test.cpp and
// stub_entity_state_client.cpp. See stub_entity_state_client.cpp for why the
// stub exists.
#pragma once

#include <cstdint>
#include <vector>

namespace simcore {

// Everything the stub observed, plus the scripted reply it hands back.
// Reset it with resetStub() so one test's traffic cannot leak into the next.
struct StubEntityStoreControl {
  // Stands in for the real client's private connected_ member, which test
  // code cannot reach. false reproduces the unconnected path exactly.
  bool connected = false;

  struct LoadCall {
    int32_t dimension = 0, x = 0, y = 0, z = 0;
    uint16_t entity_type = 0;
  };
  struct SaveCall {
    int32_t dimension = 0, x = 0, y = 0, z = 0;
    uint16_t entity_type = 0;
    std::vector<uint8_t> state;
  };

  std::vector<LoadCall> load_calls;
  std::vector<SaveCall> save_calls;

  // Scripted reply for the next LoadEntityState.
  //   state      -> EntityStateData::state handed to the callback
  //   save_ok    -> bool handed to the SaveEntityState callback
  std::vector<uint8_t> load_state;
  bool save_ok = true;

  void reset() {
    connected = false;
    load_calls.clear();
    save_calls.clear();
    load_state.clear();
    save_ok = true;
  }
};

extern StubEntityStoreControl g_stub;

} // namespace simcore
