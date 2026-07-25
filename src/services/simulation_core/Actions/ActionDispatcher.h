#pragma once
#include <cstdint>
#include <flatbuffers/verifier.h>
#include <functional>
#include <memory>
#include <vector>

namespace simcore {

using ItemGiveCallback = std::function<void(
    uint64_t player_id, uint16_t item_id, uint8_t count, int32_t target_slot)>;

class ActionDispatcher {
public:
  explicit ActionDispatcher(ItemGiveCallback onGiveItem = nullptr);

  void dispatch(const std::vector<uint8_t> &data);

private:
  bool tryParseAsPlayerAction(const std::vector<uint8_t> &data,
                              flatbuffers::Verifier &verifier);
  ItemGiveCallback onGiveItem_;
};

} // namespace simcore