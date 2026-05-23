#pragma once

#include <asio.hpp>
#include <atomic>
#include <memory>
#include <string>
#include "UI/Window.h"
#include "Camera/Camera.h"
#include "Common/Types.h"
#include "Common/NamedThreadPool.h"
#include "Network/NetClient.h"
#include "World/World.h"
#include "Cache/ChunkLoadManager.h"
#include "UI/InputManager.h"
#include "World/InteractionSystem.h"
#include "Cache/MeshManager.h"
#include "Common/Inventory.h"
#include "UI/UIManager.h"
#include "Render/RenderBridge.h"

class GameClient {
public:
    GameClient();
    ~GameClient();

    bool Init(const std::string &shaderDir, int width, int height,
              const std::string& server_host, int server_port, int bulk_port = 7778);
    void Run();
    void RequestShutdown();

private:
    void subscribeNetClient();
    void Update(float dt);

    // ---- Infrastructure ----
    asio::io_context worldContext_;
    asio::executor_work_guard<
        asio::io_context::basic_executor_type<std::allocator<void>, 0>> workGuard_;
    std::shared_ptr<NetClient> netClient_;
    NamedThreadPool &threadPool_ = NamedThreadPool::instance();

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
    InventoryState invState_;       // shared player inventory (40 slots)
    UIManager uiMgr_;               // owns all IUIWindow instances

    // ---- Transient state ----
    std::string shaderDir_;
    int width_ = 1280, height_ = 720;
    glm::vec3 prevCameraPos_{256.0f, 80.0f, 224.0f};
    std::atomic<bool> shuttingDown_{false};
};
