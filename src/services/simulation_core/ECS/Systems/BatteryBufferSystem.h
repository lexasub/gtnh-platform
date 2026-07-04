#pragma once
#include "../../Network/PipeEnergyClient.h"
#include "../components/ItemEnergyStorage.h"
#include "ECS/components/BatteryBufferComponent.h"
#include "ECS/components/InventoryContainer.h"
#include "ISystem.h"
#include <deque>
#include <entt/entt.hpp>
#include <memory>
#include <unordered_map>

namespace simcore {

class BatteryBufferSystem : public ISystem {
public:
  explicit BatteryBufferSystem(
      entt::registry &registry,
      std::shared_ptr<PipeEnergyClient> pipeClient = nullptr)
      : m_registry(registry), pipeClient_(std::move(pipeClient)) {}
  void tick(float dt) override;

  bool onConsumeResponse(uint64_t node_id, int32_t consumed, int32_t remaining);

private:
  void chargeSlot(BatteryBufferComponent &buffer, InventoryContainer &inv,
                  uint8_t slotIdx);
  entt::registry &m_registry;
  std::shared_ptr<PipeEnergyClient> pipeClient_;
  std::unordered_map<uint64_t, int32_t> pendingRequests_;
  std::deque<uint64_t> pendingOrder_;
};

} // namespace simcore