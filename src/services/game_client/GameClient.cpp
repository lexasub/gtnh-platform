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
#include "UI/Windows/block/MachineWindow.h"
#include "Common/BlockType.h"
#include "core_generated.h"
#include "machine_registry/MachineRegistry.h"
#include <limits>

GameClient::GameClient()
    : workGuard_(asio::make_work_guard(worldContext_))
    , chunkLoadWorkGuard_(asio::make_work_guard(chunkLoadContext_)) {}

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

    // 4. Stop both worker threads
    worldContext_.stop();
    chunkLoadContext_.stop();
    workGuard_.reset();
    chunkLoadWorkGuard_.reset();
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
    // World mutations (block updates, chunks) go through worldContext_ for
    // thread safety — ChunkLoadManager has its own chunkLoadContext_ and
    // won't stall this thread.
    netClient_->SetBlockUpdateCallback(
        [this](BlockPos pos, uint16_t block_id, uint8_t meta, uint32_t mb_id) {
            asio::post(worldContext_, [this, pos, block_id, meta, mb_id]() {
                meshMgr_.OnBlockUpdate(pos, block_id, meta, mb_id, world_);
            });
        });

    netClient_->SetBlockAckCallback(
        [this](BlockPos pos, uint8_t status, uint16_t block_id, uint8_t meta, uint32_t request_id, [[maybe_unused]] uint8_t action_type) {
            if (status != static_cast<uint8_t>(Protocol::BlockAckStatus_ACCEPTED)) {
                spdlog::warn("BlockAck CONFLICT at ({},{},{}) actual_id={} rid={}", pos.x, pos.y, pos.z, block_id, request_id);
            }
            // Apply + rebuild mesh on main thread so the next raycaster frame
            // sees the change immediately (BlockChangedEvent is skipped back
            // to the source player, so BlockAck is the only signal).
            meshMgr_.OnBlockUpdate(pos, block_id, meta, 0, world_);
            world_.ClearBlockActionPending(pos);
        });

    // Server-authoritative UI/effect directive — the client opens windows
    // ONLY when the server says so (BlockDirective::OPEN_UI).
    netClient_->SetBlockActionDirectiveCallback(
        [this](BlockPos pos, uint8_t directive, uint16_t block_id, uint32_t request_id, [[maybe_unused]] uint8_t action_type) {
            if (directive == static_cast<uint8_t>(Protocol::BlockDirective_OPEN_UI)) {
                UIDefaults::TryOpenBlockUI(uiMgr_, block_id, pos);
            } else if (directive == static_cast<uint8_t>(Protocol::BlockDirective_PLAY_ANIMATION)) {
                spdlog::info("BlockActionDirective: effect={} at ({},{},{}) rid={}",
                             block_id, pos.x, pos.y, pos.z, request_id);
            }
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
    // Server-driven recipe query replies → ServerRecipeDB (parse + cache).
    netClient_->SetRecipeCheckRespCallback(
        [this](std::shared_ptr<std::vector<uint8_t>> data) {
            recipeDb_.HandleRecipeResponse(GatewayMsg::kRecipeCheckResp, std::move(data));
        });
    netClient_->SetRecipeCatalogRespCallback(
        [this](std::shared_ptr<std::vector<uint8_t>> data) {
            recipeDb_.HandleRecipeResponse(GatewayMsg::kRecipeCatalogResp, std::move(data));
        });
    netClient_->SetRecipesForItemRespCallback(
        [this](std::shared_ptr<std::vector<uint8_t>> data) {
            recipeDb_.HandleRecipeResponse(GatewayMsg::kRecipeItemResp, std::move(data));
        });
    netClient_->SetRecipesForMachineRespCallback(
        [this](std::shared_ptr<std::vector<uint8_t>> data) {
            recipeDb_.HandleRecipeResponse(GatewayMsg::kRecipeMachineResp, std::move(data));
        });
    netClient_->SetMultiblockEventCallback(
        [this](std::shared_ptr<std::vector<uint8_t>> data) {
            uiMgr_.HandleNetwork(GatewayMsg::kMultiblockEvent, data->data());
        });
    netClient_->SetQuestUpdateCallback(
        [this](uint8_t msgType, std::shared_ptr<std::vector<uint8_t>> data) {
            uiMgr_.HandleNetwork(msgType, data->data());
        });

    netClient_->SetStartScenarioRespCallback(
        [this](std::shared_ptr<std::vector<uint8_t>> data) {
            uiMgr_.HandleNetwork(GatewayMsg::kStartScenarioResp, data->data());
        });

    netClient_->SetToolActionRespCallback(
        [this](bool success, uint8_t newRole, const std::vector<uint8_t>& allRoles) {
            if (!success) {
                spdlog::warn("[ToolAction] failed new_role={}", newRole);
                return;
            }
            // Rebuild mesh at the last targeted position to reflect face texture changes
            BlockPos pos = interaction_.GetHighlightedBlock();
            if (pos.x == std::numeric_limits<int32_t>::max()) return;
            uint16_t blockId = world_.GetBlockAt(pos);
            asio::post(worldContext_, [this, pos, blockId]() {
                meshMgr_.OnBlockUpdate(pos, blockId, 0, 0, world_);
            });
        });

    netClient_->SetReconnectCallback([this]() {
        world_.ClearPendingRequests();
        spdlog::info("Cleared pending chunk requests after bulk reconnect");
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
        } else {
            spdlog::warn("Machine registry empty or failed to load from {}", yaml_path);
        }

        // Multiblock controllers are runtime-registered (not yet in
        // machines.yaml — TODO: replace legacy 1001-1006 with hierarchical ids
        // when the registry is regenerated). Must happen before
        // LoadFromRegistry so right-click opens a MachineWindow for them.
        if (auto* mreg = MachineRegistry::instance()) {
            auto registerController = [mreg](uint16_t id, const char* name,
                                             const char* cls, MachineRole role,
                                             int slots_in, int slots_out) {
                MachineInfo info{};
                info.id = id;
                info.name = name;
                info.machine_class = cls;
                info.role = role;
                info.tier = 1;
                // EBF/LCR: the 4+4 slot grids map 1:1 onto the ITEM_IN/ITEM_OUT
                // hatch slot ranges by index. Boiler: 4 fuel slots in controller.
                info.slots_in = slots_in;
                info.slots_out = slots_out;
                info.capacity = 10000;
                info.maxInput = 32;
                info.maxOutput = 32;
                mreg->Register(info);
            };
            registerController(1003, "electric_blast_furnace", "ebf",
                               MachineRole::CONSUMER, 4, 4);
            registerController(1005, "large_boiler", "large_boiler",
                               MachineRole::PRODUCER, 4, 0);
            registerController(1006, "large_chemical_reactor", "chemical_reactor",
                               MachineRole::CONSUMER, 4, 4);
        }
        BlockUIFactory::LoadFromRegistry(*MachineRegistry::instance());
    }

    // ── Network ──────────────────────────────────────────────────────────
    netClient_ = std::make_shared<NetClient>();

    // ── UI system init ───────────────────────────────────────────────────
    invState_.player_id = 1; // hardcoded dev ID until auth
    UIDefaults::RegisterPlayerUI(uiMgr_, invState_);
    uiMgr_.SetNetClient(netClient_.get());

    // Server-driven recipe store (catalog + LRU caches)
    recipeDb_.Init(netClient_.get());
    uiMgr_.SetRecipeDb(&recipeDb_);

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
    camera_.SetBinder(&uiMgr_.GetBinder());
    camera_.SetWorld(&world_);
    interaction_.SetBinder(&uiMgr_.GetBinder());

    chunkLoadManager_ = std::make_unique<ChunkLoadManager>(world_, *netClient_);

    subscribeNetClient();

    if (!netClient_->Connect(server_host, static_cast<uint16_t>(server_port), static_cast<uint16_t>(bulk_port))) {
        spdlog::error("GameClient: failed to connect to server");
        return false;
    }
    // Prime the recipe catalog ("what recipes exist") once connected.
    recipeDb_.RequestCatalog();
    threadPool_.addThread(worldContext_, "ClientWorld");
    threadPool_.addThread(chunkLoadContext_, "ChunkLoad");

    return true;
}

void GameClient::Update(float dt) {
    // Flight only in CREATIVE and SPECTATOR; SURVIVAL/ADVENTURE walk only
    camera_.SetFlightEnabled(invState_.gameMode == GameMode::CREATIVE ||
                             invState_.gameMode == GameMode::SPECTATOR);

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

    // ── Right-click → server decides (open UI, place, or reject) ───────────
    // Client sends raw intent (button + target + held item + face); the server
    // answers with a BlockActionDirective (OPEN_UI) or a world-state BlockAck.
    bool rightClickHandled = false;
    if (inputMgr_.State().mouseRightPressed && !uiMgr_.AnyOpen()) {
        BlockPos target = interaction_.RaycastTarget(camera_);
        if (target.x != std::numeric_limits<int32_t>::max() &&
            world_.GetBlockAt(target) != 0) {
            netClient_->SendBlockAction(
                Protocol::PlayerActionType::PlayerActionType_RIGHT_MOUSE_CLICK,
                target.x, target.y, target.z,
                world_.GetBlockAt(target),
                interaction_.GetHeldItem(),
                interaction_.TargetFace(camera_),
                invState_.player_id);
            world_.MarkBlockActionSent(target);
            rightClickHandled = true;
        }
    }

    // ── World interaction (block break/place, only if UI not capturing) ──
    // Skip when the GameClient already sent a right-click for an interactive
    // block above — otherwise both code-paths fire duplicate SendBlockActions.
    if (!uiMgr_.AnyOpen()
        && invState_.gameMode != GameMode::ADVENTURE
        && invState_.gameMode != GameMode::SPECTATOR
        && !rightClickHandled) {
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

    asio::post(chunkLoadContext_,
               [this, frustum, pos = camera_.pos, fwd = camera_.GetForward(),
                vel = velocity, dt]() {
                  chunkLoadManager_->Update(frustum, pos, fwd, vel, dt);
               });

    // Safety net: clear pending block actions older than 100ms
    static double sweepTimer = 0;
    sweepTimer += dt;
    if (sweepTimer >= 0.1) {
        sweepTimer = 0;
        asio::post(worldContext_, [this]() {
            world_.ClearExpiredBlockActions(std::chrono::milliseconds(100));
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
