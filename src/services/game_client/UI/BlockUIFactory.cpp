#include "BlockUIFactory.h"
#include "UIManager.h"

#include "Windows/player/ClientCraftingWindow.h"
#include "Windows/block/MachineWindow.h"
#include "Windows/block/ChestWindow.h"
#include "Network/NetClient.h"

#include <common/ItemId.h>
#include <algorithm>

BlockUIFactory::Registry& BlockUIFactory::GetRegistry() {
    static Registry reg = []() {
        Registry r;
        auto registerCraftingTable = [&r](uint16_t blockId) {
            r[blockId] = [](UIManager& mgr, BlockPos pos) -> IUIWindow* {
                auto* win = FindOrCreate<CraftingWindow>(mgr, pos, mgr.GetNetClient(), &mgr.GetDragManager(), mgr.GetRecipeDb());
                if (auto* nc = mgr.GetNetClient()) {
                    nc->SetCraftResponseCallback(
                        [win](bool s, uint16_t id, uint8_t cnt, uint16_t m, const std::string& e, const std::array<ItemStack, 9>& grid) {
                            win->OnCraftResponse(s, id, cnt, m, e, grid);
                        });
                }
                // Server-authoritative workbench.open needs the player id; use
                // the factory value (lastPlayerInv_ is null on first open).
                if (auto* pinv = mgr.GetPlayerInventory()) {
                    win->SetPlayerId(pinv->player_id);
                }
                return win;
            };
        };
        // The real crafting-table block id is the packed hierarchical
        // "0:10:11:1" (22529) — the flat 14 is a legacy alias. The block is
        // also listed in machines.yaml, so without this override LoadFromRegistry
        // would open a MachineWindow (flat row of slots) instead of the 3×3
        // crafting grid.
        registerCraftingTable(ItemId::pack("0:10:11:1"));
        registerCraftingTable(static_cast<uint16_t>(BlockType::CraftingTable));
        // Chest: register both legacy flat ID (37) and actual packed ID (0:10:11:0 → 22528)
        auto registerChest = [&r](uint16_t blockId) {
            r[blockId] = [](UIManager& mgr, BlockPos pos) -> IUIWindow* {
                auto* win = FindOrCreate<ChestWindow>(mgr, pos);
                if (win) {
                    win->SetDragManager(&mgr.GetDragManager());
                    win->SetNetClient(mgr.GetNetClient());
                    // Server-authoritative chest.open needs the player id; use
                    // the factory value (lastPlayerInv_ is null on first open).
                    if (auto* pinv = mgr.GetPlayerInventory()) {
                        win->SetPlayerId(pinv->player_id);
                    }
                }
                return win;
            };
        };
        registerChest(static_cast<uint16_t>(BlockType::Chest));          // 37 (legacy)
        registerChest(ItemId::pack("0:10:11:0"));                       // 22528 (packed)
        return r;
    }();
    return reg;
}

bool BlockUIFactory::CanOpen(uint16_t blockId) {
    return GetRegistry().contains(blockId);
}

IUIWindow* BlockUIFactory::Create(uint16_t blockId, BlockPos pos, UIManager& mgr) {
    auto& reg = GetRegistry();
    auto it = reg.find(blockId);
    if (it != reg.end()) {
        return it->second(mgr, pos);
    }
    return nullptr;
}

void BlockUIFactory::RegisterBlock(uint16_t blockId, Creator creator) {
    GetRegistry()[blockId] = std::move(creator);
}

std::vector<uint16_t> BlockUIFactory::All() {
    std::vector<uint16_t> types;
    for (auto& [type, _] : GetRegistry()) {
        types.push_back(type);
    }
    std::sort(types.begin(), types.end());
    return types;
}

IUIWindow* BlockUIFactory::FindOrCreateMachine(UIManager& mgr, BlockPos pos, uint16_t type) {
    auto* win = FindOrCreate<MachineWindow>(mgr, pos, type);
    if (win) {
        win->SetNetClient(mgr.GetNetClient());
        win->SetDragManager(&mgr.GetDragManager());
        // Server-authoritative machine.open needs the player id; use the
        // factory value (lastPlayerInv_ is null on first open).
        if (auto* pinv = mgr.GetPlayerInventory()) {
            win->SetPlayerId(pinv->player_id);
        }
    }
    return win;
}

void BlockUIFactory::LoadFromRegistry(const MachineRegistry& reg) {
    for (auto& [id, info] : reg.All()) {
        // Keep manually-registered custom windows (e.g. CraftingWindow for the
        // crafting table) — LoadFromRegistry only fills in gaps.
        if (GetRegistry().contains(id)) continue;
        GetRegistry()[id] = [id](UIManager& mgr, BlockPos pos) -> IUIWindow* {
            return FindOrCreateMachine(mgr, pos, id);
        };
    }
}

// FindOrCreate is defined in BlockUIFactory.h (template)
