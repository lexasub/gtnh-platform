#pragma once
#include "apps/simcore/Network/PipeEnergyClient.h"
#include "apps/simcore/Network/IEventPublisher.h"
#include <game/machines/ItemEnergyStorage.h>
#include <engine/sim/components/BatteryBufferComponent.h>
#include <engine/sim/components/InventoryContainer.h>
#include <engine/sim/ISystem.h>
#include <deque>
#include <entt/entt.hpp>
#include <memory>
#include <unordered_map>

namespace simcore {

class BatteryBufferSystem : public ISystem {
public:
  explicit BatteryBufferSystem(
      entt::registry &registry,
      std::shared_ptr<PipeEnergyClient> pipeClient = nullptr,
      std::shared_ptr<IEventPublisher> events = nullptr)
      : m_registry(registry), pipeClient_(std::move(pipeClient)),
        events_(std::move(events)) {}
  void tick(float dt) override;

  bool onConsumeResponse(uint64_t node_id, int32_t consumed, int32_t remaining);

private:
  void chargeSlot(BatteryBufferComponent &buffer, InventoryContainer &inv,
                  uint8_t slotIdx);
  entt::registry &m_registry;
  std::shared_ptr<PipeEnergyClient> pipeClient_;
  std::shared_ptr<IEventPublisher> events_;
  // Outstanding consume requests, keyed by the node id that was sent on the
  // wire. Node id 0 is a REAL node (entt's first entity), so the map — not a
  // sentinel in the key — is what distinguishes "we asked" from "we did not".
  // There is no ordering queue: a response is correlated by node id alone
  // (gp-u9ua).
  std::unordered_map<uint64_t, int32_t> pendingRequests_;
};

} // namespace simcore