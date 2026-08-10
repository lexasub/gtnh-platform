#include "CraftRequestHandler.h"
#include "../Common/MainThreadQueue.h"
#include "../Network/IEventPublisher.h"
#include "../Network/clients/IoUringRouterClient.h"
#include "../Storage/PlayerInventoryStore.h"
#include "../Quest/QuestManager.h"
#include "../RecipeManager/RecipeManager.h"
#include "WorkbenchStateManager.h"
#include "core_generated.h"
#include "recipe_generated.h"
#include <common/ItemId.h>
#include <spdlog/spdlog.h>

namespace simulation_core {

CraftRequestHandler::CraftRequestHandler(std::shared_ptr<simcore::IoUringRouterClient> router,
                                         std::shared_ptr<RecipeManager::RecipeManager> recipeManager,
                                         std::shared_ptr<simcore::PlayerInventoryStore> inventoryStore,
                                         std::shared_ptr<simcore::QuestManager> questManager,
                                         std::shared_ptr<WorkbenchStateManager> wbStateManager,
                                         simcore::MainThreadQueue* mainQueue,
                                         std::shared_ptr<simcore::IEventPublisher> eventPublisher)
    : router_(std::move(router)), recipeManager_(std::move(recipeManager)),
      inventoryStore_(std::move(inventoryStore)), questManager_(std::move(questManager)),
      wbStateManager_(std::move(wbStateManager)),
      mainQueue_(mainQueue), eventPublisher_(std::move(eventPublisher))
{}

void CraftRequestHandler::handle(const std::vector<uint8_t>& data) {
    flatbuffers::Verifier v(data.data(), data.size());
    if (!v.VerifyBuffer<Protocol::CraftRequest>(nullptr)) {
        spdlog::warn("[CraftRequest] invalid buffer");
        return;
    }
    auto req = flatbuffers::GetRoot<Protocol::CraftRequest>(data.data());
    uint64_t playerId = req->player_id();
    if (!req->pos()) {
        spdlog::warn("[CraftRequest] missing pos field");
        return;
    }
    int32_t x = req->pos()->x(), y = req->pos()->y(), z = req->pos()->z();

    // Server-authoritative: ignore client-supplied slots. Read grid from
    // WorkbenchStateManager (cache-first, ESS async on miss).
    if (!wbStateManager_) {
        spdlog::warn("[CraftRequest] no WorkbenchStateManager — cannot craft");
        return;
    }
    wbStateManager_->getGridState(x, y, z,
        [this, playerId, x, y, z](const std::vector<RecipeManager::ItemStack>& grid) {
            // Callback may fire on ESS io thread (cache miss) or immediately
            // (cache hit). Ensure doCraft runs on the main queue.
            if (mainQueue_) {
                mainQueue_->push([this, playerId, x, y, z, grid]() {
                    doCraft(playerId, x, y, z, grid);
                });
            } else {
                doCraft(playerId, x, y, z, grid);
            }
        });
}

void CraftRequestHandler::doCraft(uint64_t playerId, int32_t x, int32_t y, int32_t z,
                                  const std::vector<RecipeManager::ItemStack>& grid) {
    if (grid.empty()) {
        flatbuffers::FlatBufferBuilder err(64);
        auto errStr = err.CreateString("Workbench grid is empty");
        auto resp = Protocol::CreateCraftResponse(err, false, nullptr, errStr);
        err.Finish(resp);
        router_->Publish("sim.craft.response",
            {err.GetBufferPointer(), err.GetBufferPointer() + err.GetSize()});
        return;
    }

    // crafting_table block_id from machines.yaml ("0:10:11:1" → packed).
    constexpr uint16_t kCraftingTableMachineId = ItemId::pack("0:10:11:1");
    const auto* recipe = recipeManager_->findRecipeByInputs(kCraftingTableMachineId, grid);
    if (!recipe) {
        flatbuffers::FlatBufferBuilder err(64);
        auto errStr = err.CreateString("No matching recipe");
        auto resp = Protocol::CreateCraftResponse(err, false, nullptr, errStr);
        err.Finish(resp);
        router_->Publish("sim.craft.response",
            {err.GetBufferPointer(), err.GetBufferPointer() + err.GetSize()});
        return;
    }

    auto originalGrid = grid;
    auto newGrid = recipe->consumeInputs(grid);

    // Persist consumed grid to ESS + cache.
    if (wbStateManager_) {
        wbStateManager_->setGridState(x, y, z, newGrid);
    }

    // Publish updated grid to client (server-authoritative snapshot).
    if (eventPublisher_) {
        eventPublisher_->publishGridUpdate(x, y, z, newGrid);
    }

    // Deduct consumed items from player inventory.
    {
        auto inv = inventoryStore_->getSlots(playerId);
        for (size_t i = 0; i < 9 && i < originalGrid.size(); ++i) {
            auto& orig = originalGrid[i];
            auto& cons = newGrid[i];
            if (orig.item_id == 0) continue;
            int consumedCount = static_cast<int>(orig.count) - static_cast<int>(cons.count);
            if (consumedCount <= 0) continue;
            int remaining = consumedCount;
            for (auto& slot : inv) {
                if (remaining <= 0) break;
                if (slot.item_id == orig.item_id) {
                    int deduct = (remaining < static_cast<int>(slot.count)) ? remaining : static_cast<int>(slot.count);
                    slot.count -= static_cast<uint8_t>(deduct);
                    remaining -= deduct;
                    if (slot.count == 0) slot.item_id = 0;
                }
            }
        }
        inventoryStore_->setSlots(playerId, inv);
    }

    RecipeManager::ItemStack result{0, 0, 0};
    if (!recipe->outputs.empty()) {
        const auto& out = recipe->outputs[0];
        result = {out.item_id, out.count, out.metadata};
    }

    // Send CraftResponse with updated grid so client can redraw.
    {
        flatbuffers::FlatBufferBuilder fb(256);
        Protocol::ItemStack fbResult(result.item_id, result.count, result.metadata);
        std::vector<Protocol::ItemStack> fbGrid;
        fbGrid.reserve(9);
        for (auto& gs : newGrid)
            fbGrid.push_back(Protocol::ItemStack(gs.item_id, gs.count, gs.metadata));
        auto gridVec = fb.CreateVectorOfStructs<Protocol::ItemStack>(fbGrid);
        auto resp = Protocol::CreateCraftResponse(fb, true, &fbResult,
                                                   fb.CreateString(""), gridVec);
        fb.Finish(resp);
        router_->Publish("sim.craft.response",
            {fb.GetBufferPointer(), fb.GetBufferPointer() + fb.GetSize()});
    }

    if (result.item_id != 0) {
        inventoryStore_->giveItem(playerId, result.item_id, result.count, -1);
        if (questManager_) {
            questManager_->checkCraftCompletion(playerId, result.item_id, result.count);
        }
    }

    spdlog::info("CraftRequest: {} -> item {} x{}", recipe->id, result.item_id, result.count);
}

} // namespace simcore
