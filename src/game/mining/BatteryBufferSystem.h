#pragma once
#include "apps/simcore/Network/PipeEnergyClient.h"
#include "apps/simcore/Network/IEventPublisher.h"
#include <game/machines/ItemEnergyStorage.h>
#include <game/machines/BatteryBufferComponent.h>
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
  std::unordered_map<uint64_t, int32_t> pendingRequests_;
  std::deque<uint64_t> pendingOrder_;
};

} // namespace simcore