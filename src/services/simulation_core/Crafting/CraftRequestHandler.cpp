#include "CraftRequestHandler.h"
#include "../Network/clients/IoUringRouterClient.h"
#include "../Storage/PlayerInventoryStore.h"
#include "../RecipeManager/RecipeManager.h"
#include "core_generated.h"
#include "recipe_generated.h"
#include <spdlog/spdlog.h>

namespace simcore {

CraftRequestHandler::CraftRequestHandler(std::shared_ptr<IoUringRouterClient> router,
                                         std::shared_ptr<RecipeManager::RecipeManager> recipeManager,
                                         std::shared_ptr<PlayerInventoryStore> inventoryStore)
    : router_(std::move(router)), recipeManager_(std::move(recipeManager)),
      inventoryStore_(std::move(inventoryStore))
{}

void CraftRequestHandler::handle(const std::vector<uint8_t>& data) {
    flatbuffers::Verifier v(data.data(), data.size());
    if (!v.VerifyBuffer<Protocol::CraftRequest>(nullptr)) {
        spdlog::warn("[CraftRequest] invalid buffer");
        return;
    }
    auto req = flatbuffers::GetRoot<Protocol::CraftRequest>(data.data());
    auto slots = req->slots();
    if (!slots) {
        spdlog::warn("[CraftRequest] missing slots field");
        return;
    }
    uint64_t playerId = req->player_id();

    uint16_t n = std::min<uint16_t>(slots->size(), 9);
    std::vector<RecipeManager::ItemStack> grid;
    grid.reserve(n);
    for (uint16_t i = 0; i < n; ++i) {
        auto* s = slots->Get(i);
        grid.push_back(s ? RecipeManager::ItemStack{
            static_cast<uint16_t>(s->item_id()),
            static_cast<uint8_t>(s->count()),
            s->meta()
        } : RecipeManager::ItemStack{0, 0, 0});
    }

    constexpr uint16_t kCraftingTableMachineId = 14;
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
    grid = recipe->craft(grid);

    {
        auto inv = inventoryStore_->getSlots(playerId);
        for (size_t i = 0; i < 9 && i < originalGrid.size(); ++i) {
            auto& orig = originalGrid[i];
            auto& cons = grid[i];
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

    {
        flatbuffers::FlatBufferBuilder fb(256);
        Protocol::ItemStack fbResult(result.item_id, result.count, result.metadata);
        std::vector<Protocol::ItemStack> fbGrid;
        fbGrid.reserve(9);
        for (auto& gs : grid)
            fbGrid.push_back(Protocol::ItemStack(gs.item_id, gs.count, gs.metadata));
        auto gridVec = fb.CreateVectorOfStructs<Protocol::ItemStack>(fbGrid);
        auto resp = Protocol::CreateCraftResponse(fb, true, &fbResult,
                                                   fb.CreateString(""), gridVec);
        fb.Finish(resp);
        router_->Publish("sim.craft.response",
            {fb.GetBufferPointer(), fb.GetBufferPointer() + fb.GetSize()});
    }

    if (result.item_id != 0)
        inventoryStore_->giveItem(playerId, result.item_id, result.count, -1);

    spdlog::info("CraftRequest: {} -> item {} x{}", recipe->id, result.item_id, result.count);
}

} // namespace simcore
