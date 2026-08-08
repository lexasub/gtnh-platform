#pragma once
#include "Actions/Callbacks.h"
#include <cstdint>
#include <flatbuffers/verifier.h>
#include <vector>

namespace simcore {

// Dispatcher for the "player.actions" topic (legacy PlayerAction frames —
// ITEM_ACTION / CHUNK_REQUEST). Distinct from ActionDispatcher, which routes
// SetBlockAction frames through the typed block-action handlers.
class PlayerActionDispatcher {
public:
  explicit PlayerActionDispatcher(ItemGiveCallback onGiveItem = nullptr);

  void dispatch(const std::vector<uint8_t>& data);

private:
  bool tryParseAsPlayerAction(const std::vector<uint8_t>& data,
                              flatbuffers::Verifier& verifier);
  ItemGiveCallback onGiveItem_;
};

} // namespace simcore
