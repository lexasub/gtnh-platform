#pragma once
#include <cstdint>

namespace simcore {

class ActionContext;

// Right-click on a chest (packed 0:10:11:0): ack, open the UI and stream the
// chest inventory from EntityStateStore so the client window opens populated.
class ChestInteractHandler {
public:
  bool canHandle(const ActionContext& ctx) const;
  void handle(const ActionContext& ctx) const;

private:
  static constexpr uint16_t kChestEntityType = 3;
};

} // namespace simcore
