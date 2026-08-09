#pragma once
#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace simcore {

class MainThreadQueue;
class SimulationEngine;
class IoUringRouterClient;
class RouterEventPublisher;
class PipeEnergyClient;
class FluidClient;
class ItemClient;
class PlayerInventoryStore;
class EntityStateStoreClient;
class ChunkStoreRepository;
class TopicDispatcher;
class PlayerActionDispatcher;
class SetBlockCASHandler;
class ChunkEventHandler;
class WrenchHandler;
class WorldContainerInventory;
class IoUringChunkClient;
class QuestManager;
class MachineSystem;
class BatteryBufferSystem;

} // namespace simcore

namespace RecipeManager { class RecipeManager; }

namespace simulation_core { class WorkbenchStateManager; }

namespace simcore {

class SimCoreMessageHandler {
public:
  struct Deps {
    MainThreadQueue* mainQueue = nullptr;
    std::shared_ptr<SimulationEngine> engine;
    std::shared_ptr<IoUringRouterClient> routerClient;
    std::shared_ptr<RouterEventPublisher> eventPublisher;
    std::shared_ptr<PipeEnergyClient> pipeEnergyClient;
    std::shared_ptr<FluidClient> fluidClient;
    std::shared_ptr<ItemClient> itemClient;
    std::shared_ptr<PlayerInventoryStore> inventoryStore;
    std::shared_ptr<EntityStateStoreClient> entityStateClient;
    std::shared_ptr<RecipeManager::RecipeManager> recipeManager;
    std::shared_ptr<ChunkStoreRepository> blockRepository;
    std::shared_ptr<WrenchHandler> wrenchHandler;
    std::shared_ptr<IoUringChunkClient> chunkClient;
    std::shared_ptr<QuestManager> questManager;
    MachineSystem* machineSystem = nullptr;
    BatteryBufferSystem* batteryBuffer = nullptr;
    std::shared_ptr<simulation_core::WorkbenchStateManager> wbStateManager;
  };

  explicit SimCoreMessageHandler(Deps deps);
  void setup();
  void wireOnMessage(WorldContainerInventory& worldContainers);
  void subscribeAll();

private:
  Deps deps_;
  std::shared_ptr<TopicDispatcher> topicDispatcher_;
  std::shared_ptr<PlayerActionDispatcher> dispatcher_;
  std::shared_ptr<SetBlockCASHandler> casHandler_;
  std::shared_ptr<ChunkEventHandler> chunkHandler_;
};

} // namespace simcore
