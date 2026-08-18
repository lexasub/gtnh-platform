#pragma once

#include "Cache/ChunkLoadManager.h"
#include "Cache/MeshManager.h"
#include "Camera/Camera.h"
#include "Common/Inventory.h"
#include "Common/NamedThreadPool.h"
#include "Common/Types.h"
#include "Crafting/ServerRecipeDB.h"
#include "Network/NetClient.h"
#include "Render/RenderBridge.h"
#include "UI/InputManager.h"
#include "UI/UIManager.h"
#include "UI/Window.h"
#include "World/InteractionSystem.h"
#include "World/World.h"
#include <asio.hpp>
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <cstdint>

// Forward declare for pipe fluid cache key — same layout as PipeNetwork::posKey.
inline uint64_t pipePosKeyFlat(int32_t x, int32_t y, int32_t z) {
    return (static_cast<uint64_t>(static_cast<int64_t>(x)) << 42)
         | (static_cast<uint64_t>(static_cast<int64_t>(y) & 0xFFFFF) << 20)
         | (static_cast<uint64_t>(static_cast<int64_t>(z) & 0xFFFFF));
}

class GameClient {
public:
  GameClient();
  ~GameClient();

  bool Init(const std::string &shaderDir, int width, int height,
            const std::string &server_host, int server_port,
            int bulk_port = 7778);
  void Run();
  void RequestShutdown();

private:
  void subscribeNetClient();
  void Update(float dt);

  // ---- Infrastructure ----
  asio::io_context worldContext_;       // block updates, chunks, world mutations
  asio::io_context chunkLoadContext_;   // ChunkLoadManager (separate thread)
  asio::executor_work_guard<
      asio::io_context::basic_executor_type<std::allocator<void>, 0>>
      workGuard_;
  asio::executor_work_guard<
      asio::io_context::basic_executor_type<std::allocator<void>, 0>>
      chunkLoadWorkGuard_;
  std::shared_ptr<NetClient> netClient_;
  NamedThreadPool &threadPool_ = NamedThreadPool::instance();

  // Server-driven recipe store (catalog + LRU caches); owns recipe queries.
  ServerRecipeDB recipeDb_;

  // ---- Core subsystems ----
  GLFWWindow window_;
  Camera camera_;
  InputManager inputMgr_;
  World world_;
  InteractionSystem interaction_{&world_};
  MeshManager meshMgr_{world_};
  RenderBridge renderBridge_{&world_};
  std::unique_ptr<ChunkLoadManager> chunkLoadManager_;

  // ---- UI system (Mediator + Strategy) ----
  InventoryState invState_; // shared player inventory (40 slots)
  UIManager uiMgr_;         // owns all IUIWindow instances

  // ---- Transient state ----
  std::string shaderDir_;
  int width_ = 1280, height_ = 720;
  glm::vec3 prevCameraPos_{256.0f, 80.0f, 224.0f};
  std::atomic<bool> shuttingDown_{false};

  // Wrench guidance toast pending delivery to the render-thread HUD overlay
  // (set on ToolActionResp, drained into FrameExt once per message).
  std::string pendingHudToast_;
  float pendingHudToastLifetime_ = 0.0f;

  // Pipe fluid state cache: posKey -> {fluidId, amount, capacity}
  // Populated by fluid.pipe.state messages from PipeNetworkService.
  struct PipeFluidCacheEntry {
    uint32_t fluidId = 0;
    int32_t amount = 0;
    int32_t capacity = 0;
  };
  std::unordered_map<uint64_t, PipeFluidCacheEntry> pipeFluidCache_;
};
