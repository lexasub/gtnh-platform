#include "BlockUIFactory.h"
#include "UIManager.h"

#include "Windows/player/ClientCraftingWindow.h"
#include "Windows/block/MachineWindow.h"
#include "Windows/block/ChestWindow.h"
#include "Network/NetClient.h"

#include <algorithm>

BlockUIFactory::Registry& BlockUIFactory::GetRegistry() {
    static Registry reg = []() {
        Registry r;
        r[static_cast<uint16_t>(BlockType::CraftingTable)] = [](UIManager& mgr, BlockPos pos) -> IUIWindow* {
            auto* win = FindOrCreate<CraftingWindow>(mgr, pos, mgr.GetNetClient(), &mgr.GetDragManager());
            if (auto* nc = mgr.GetNetClient()) {
                nc->SetCraftResponseCallback(
                    [win](bool s, uint16_t id, uint8_t cnt, uint16_t m, const std::string& e, const std::array<ItemStack, 9>& grid) {
                        win->OnCraftResponse(s, id, cnt, m, e, grid);
                    });
            }
            return win;
        };
        r[static_cast<uint16_t>(BlockType::Chest)] = [](UIManager& mgr, BlockPos pos) -> IUIWindow* {
            auto* win = FindOrCreate<ChestWindow>(mgr, pos);
            if (win) {
                win->SetDragManager(&mgr.GetDragManager());
                win->SetNetClient(mgr.GetNetClient());
                if (auto* nc = mgr.GetNetClient()) {
                    nc->SetChestOpenRespCallback(
                        [win](BlockPos p, bool success, const std::vector<ItemStack>& slots) {
                            win->onChestSlotAck(p, success, slots);
                        });
                }
            }
            return win;
        };
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
        if (auto* nc = mgr.GetNetClient()) {
            nc->SetSetMachineSlotRespCallback(
                [&mgr](BlockPos p, uint8_t slotIdx, bool success, const std::string&, const ItemStack&) {
                    auto* existing = mgr.FindOpenAtBlock(p);
                    if (auto* mw = dynamic_cast<MachineWindow*>(existing)) {
                        mw->onMachineSlotAck(p.x, p.y, p.z, slotIdx, success);
                    }
                }
            );
        }
    }
    return win;
}

void BlockUIFactory::LoadFromRegistry(const MachineRegistry& reg) {
    for (auto& [id, info] : reg.All()) {
        GetRegistry()[id] = [id](UIManager& mgr, BlockPos pos) -> IUIWindow* {
            return FindOrCreateMachine(mgr, pos, id);
        };
    }
}

// FindOrCreate is defined in BlockUIFactory.h (template)
