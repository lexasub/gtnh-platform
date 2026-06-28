#include "GameClient.h"
#include <GLFW/glfw3.h>
#include <asio/executor_work_guard.hpp>
#include <spdlog/spdlog.h>
#include <glm/gtc/type_ptr.hpp>
#include <thread>
#include <chrono>

#include "World/ChunkView.h"
#include "Crafting/ClientItemRegistry.h"
#include "UI/UIDefaults.h"
#include "UI/BlockUIFactory.h"
#include "UI/Windows/player/PlayerInventory.h"
#include "UI/Windows/player/CreativeMenu.h"
#include "Common/BlockType.h"
#include "core_generated.h"
#include "machine_registry/MachineRegistry.h"
#include <limits>

GameClient::GameClient() : workGuard_(asio::make_work_guard(worldContext_)) {}

GameClient::~GameClient() {
    shuttingDown_ = true;

    // 1. Stop network — no more callbacks will fire
    netClient_->Disconnect();

    // 2. Drain pending mesh builds (they hold world_ references)
    meshMgr_.RequestShutdown();
    meshMgr_.WaitForPending();

    // 3. Disarm callbacks so straggling TBB tasks find nullptr
    netClient_->SetChunkCallback(nullptr);
    netClient_->SetBlockUpdateCallback(nullptr);
    netClient_->SetBlockAckCallback(nullptr);

    // 4. Stop world thread
    worldContext_.stop();
    workGuard_.reset();
    threadPool_.join();

    // 5. Destroy GPU resources before bgfx shutdown
    meshMgr_.DiscardHandles();

    // 6. Shutdown render library (bgfx::shutdown invalidates all handles)
    renderBridge_.Shutdown();
}

void GameClient::RequestShutdown() {
    shuttingDown_ = true;
    glfwSetWindowShouldClose(window_.Handle(), GLFW_TRUE);
}

void GameClient::subscribeNetClient() {
    // Post world mutations to worldContext_ so all ChunkStorage access
    // happens on the same thread as ChunkLoadManager.
    netClient_->SetBlockUpdateCallback(
        [this](BlockPos pos, uint16_t block_id, uint8_t meta, uint32_t mb_id) {
            asio::post(worldContext_, [this, pos, block_id, meta, mb_id]() {
                meshMgr_.OnBlockUpdate(pos, block_id, meta, mb_id, world_);
            });
        });

    netClient_->SetBlockAckCallback(
        [this](BlockPos pos, uint8_t status, uint16_t block_id, uint8_t meta) {

            if (status != static_cast<uint8_t>(Protocol::BlockAckStatus_ACCEPTED)) {
                spdlog::warn("BlockAck CONFLICT at ({},{},{}) actual_id={}", pos.x, pos.y, pos.z, block_id);
            }
            asio::post(worldContext_, [this, pos, block_id, meta] {
                meshMgr_.OnBlockUpdate(pos, block_id, meta, 0, world_);
                world_.ClearBlockActionPending(pos);
            });
        });

    netClient_->SetChunkCallback(
        [this](std::shared_ptr<ChunkView> ch, ChunkCoord coord) {
            asio::post(worldContext_, [this, coord, ch = std::move(ch)]() {
                meshMgr_.OnChunkData(coord, std::move(ch), world_);
            });
        });

    // UI network callbacks — dispatch to UIManager
    netClient_->SetInventoryUpdateCallback(
        [this](std::shared_ptr<std::vector<uint8_t>> data) {
            uiMgr_.HandleNetwork(GatewayMsg::kInventoryUpdate, data->data());
        });
    netClient_->SetBlockEntityUpdateCallback(
        [this](std::shared_ptr<std::vector<uint8_t>> data) {
            uiMgr_.HandleNetwork(GatewayMsg::kBlockEntityUpdate, data->data());
        });
    netClient_->SetRecipeCompletedCallback(
        [this](std::shared_ptr<std::vector<uint8_t>> data) {
            uiMgr_.HandleNetwork(GatewayMsg::kRecipeCompleted, data->data());
        });
}

bool GameClient::Init(const std::string& shaderDir, int width, int height,
                       const std::string& server_host, int server_port, int bulk_port) {
    shaderDir_ = shaderDir;
    width_ = width;
    height_ = height;

    if (!window_.Init(width_, height_, "GTNH GameClient")) {
        spdlog::error("Failed to init window");
        return false;
    }

    inputMgr_.Subscribe(window_);

    // ── Item registry — load items.csv ───────────────────────────────────
    ItemRegistry::Init();

    // ── Machine registry — load from machines.yaml ───────────────────────
    {
        const char* yaml_path = std::getenv("GTNH_MACHINES_YAML");
        if (!yaml_path) yaml_path = "data/registry/machines.yaml";
        auto reg = MachineRegistry::LoadFromYaml(yaml_path);
        if (reg && reg->All().size() > 0) {
            MachineRegistry::setInstance(reg.release());
            spdlog::info("Loaded machine registry from {}", yaml_path);
        }
    }
    BlockUIFactory::LoadFromRegistry(*MachineRegistry::instance());

    // ── Network ──────────────────────────────────────────────────────────
    netClient_ = std::make_shared<NetClient>();

    // ── UI system init ───────────────────────────────────────────────────
    invState_.player_id = 1; // hardcoded dev ID until auth
    UIDefaults::RegisterPlayerUI(uiMgr_, invState_);
    uiMgr_.SetNetClient(netClient_.get());

    // Wire action system (after UI registration so windows are created)
    uiMgr_.GetActions().Init(&uiMgr_.GetActionRegistry(), &uiMgr_,
                              netClient_.get(), &invState_);

    // Pass UIManager to RenderBridge so the ImGui overlay can render UI
    renderBridge_.SetUIManager(&uiMgr_);

    // ── RenderBridge init ────────────────────────────────────────────────
    renderBridge_.Init(window_.Handle(), width_, height_, shaderDir_);

    // Wire MeshManager provider into RenderLib
    renderlib::RenderAPI::SetMeshProvider(
        std::shared_ptr<renderlib::IMeshProvider>(
            meshMgr_.GetProvider(),
            [](void*) {} /* no-op deleter — owned by MeshManager */));

    camera_.Init();

    chunkLoadManager_ = std::make_unique<ChunkLoadManager>(world_, *netClient_);

    subscribeNetClient();

    if (!netClient_->Connect(server_host, static_cast<uint16_t>(server_port), static_cast<uint16_t>(bulk_port))) {
        spdlog::error("GameClient: failed to connect to server");
        return false;
    }
    threadPool_.addThread(worldContext_, "ClientWorld");

    return true;
}

void GameClient::Update(float dt) {
    if (inputMgr_.IsMouseCaptured()) {
        camera_.Update(dt, inputMgr_.State());
    }

    // ── UI input (hotbar, Escape, per-window keys) ─────────────────────
    uiMgr_.ProcessInput(inputMgr_.State());

    // ── Sync mouse capture with UI state ─────────────────────────────────
    if (uiMgr_.AnyOpen() && inputMgr_.IsMouseCaptured()) {
        inputMgr_.SetMouseCaptured(false);
    } else if (!uiMgr_.AnyOpen() && !inputMgr_.IsMouseCaptured()) {
        inputMgr_.SetMouseCaptured(true);
    }

    // ── Right-click → open block UI ────────────────────────────────────
    // Check before InteractionSystem so UI opening takes priority over placing blocks.
    if (inputMgr_.State().mouseRightPressed && !uiMgr_.AnyOpen()) {
        // Fresh ray-cast per frame — don't use stale highlightedBlock_ from last Update call
        BlockPos target = interaction_.RaycastTarget(camera_);
        if (target.x != std::numeric_limits<int32_t>::max()) {
            uint16_t blockId = world_.GetBlockAt(target);
            UIDefaults::TryOpenBlockUI(uiMgr_, blockId, target);
        }
    }

    // ── World interaction (block break/place, only if UI not capturing) ──
    if (!uiMgr_.AnyOpen()) {
        interaction_.SetInventory(&invState_);
        interaction_.Update(camera_, inputMgr_.State(), world_, *netClient_);
        // If block ack conflict occurs, invState_ gets out of sync — a future
        // server-pushed InventoryUpdate will correct it.
    }

    // ── Camera motion ──────────────────────────────────────────────────
    glm::vec3 velocity = (camera_.pos - prevCameraPos_) / dt;
    prevCameraPos_ = camera_.pos;

    Frustum frustum =
        camera_.GetFrustum(static_cast<float>(width_) / static_cast<float>(height_));

    asio::post(worldContext_,
               [this, frustum, pos = camera_.pos, fwd = camera_.GetForward(),
                vel = velocity, dt]() {
                  chunkLoadManager_->Update(frustum, pos, fwd, vel, dt);
               });

    // Safety net: clear pending block actions older than 3 seconds
    static double sweepTimer = 0;
    sweepTimer += dt;
    if (sweepTimer >= 1.0) {
        sweepTimer = 0;
        asio::post(worldContext_, [this]() {
            world_.ClearExpiredBlockActions(std::chrono::seconds(3));
        });
    }
}

void GameClient::Run() {
    auto lastTime = std::chrono::steady_clock::now();

    while (!window_.ShouldClose()) {
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        netClient_->Poll();
        window_.GlfwWaitEventsTimeout();
        inputMgr_.ClearFirstFrame();
        Update(dt);

        // Sync window size (i3wm or user may have resized the window)
        {
            int curW, curH;
            glfwGetWindowSize(window_.Handle(), &curW, &curH);
            if (curW != width_ || curH != height_) {
                width_ = curW;
                height_ = curH;
                renderlib::RenderAPI::Resize(width_, height_);
            }
        }

        // Apply completed mesh builds to GPU before rendering
        meshMgr_.ProcessPendingOps();

        renderBridge_.SetCameraPosition(camera_.pos);

        uint16_t highlightedBlockId = 0;
        if (interaction_.HasHighlight()) {
            highlightedBlockId = world_.GetBlockAt(interaction_.GetHighlightedBlock());
        }

        auto frd = RenderBridge::BuildFrameData(
            camera_, inputMgr_.State(), width_, height_, dt,
            inputMgr_.IsMouseCaptured(), interaction_.HasHighlight(),
            interaction_.GetHighlightedBlock(), highlightedBlockId,
            world_.ChunkCount(), meshMgr_.MeshCount());

        renderBridge_.SubmitFrame(frd);
        renderBridge_.WaitForFrame();
        inputMgr_.ResetFrameState();

        // Destroy GPU meshes for evicted chunks
        for (const auto& coord : world_.TakeEvictedChunks()) {
            meshMgr_.HandleEviction(coord);
        }
    }
}
