#pragma once
#include "Network/ITopicHandler.h"
#include <memory>

namespace simcore {

class ContainerSessionRegistry;
class ChestStateManager;
class IoUringRouterClient;
class PlayerInventoryStore;
class SimulationEngine;
class ChunkStoreRepository;
class MainThreadQueue;

// Handles a machine window opening: loads saved machine slots (async via
// ChestStateManager → EntityStateStore, blob keyed by machine_id), rewrites
// the LIVE ECS InventoryContainer, registers the per-player machine session
// and publishes the full InventoryUpdate snapshot (container_id=1).
class MachineOpenHandler : public ITopicHandler {
public:
  MachineOpenHandler(std::shared_ptr<ContainerSessionRegistry> sessions,
                     std::shared_ptr<ChestStateManager> stateMgr,
                     std::shared_ptr<PlayerInventoryStore> inv,
                     std::shared_ptr<IoUringRouterClient> router,
                     std::shared_ptr<SimulationEngine> engine,
                     std::shared_ptr<ChunkStoreRepository> chunkStore,
                     MainThreadQueue* mainQueue);
  void handle(const std::vector<uint8_t>& data) override;

private:
  std::shared_ptr<ContainerSessionRegistry> sessions_;
  std::shared_ptr<ChestStateManager> stateMgr_;
  std::shared_ptr<PlayerInventoryStore> inv_;
  std::shared_ptr<IoUringRouterClient> router_;
  std::shared_ptr<SimulationEngine> engine_;
  std::shared_ptr<ChunkStoreRepository> chunkStore_;
  MainThreadQueue* mainQueue_;
};

} // namespace simcore
