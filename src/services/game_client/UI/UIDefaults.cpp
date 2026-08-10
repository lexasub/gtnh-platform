#include "UIDefaults.h"
#include "UIManager.h"
#include "BlockUIFactory.h"
#include "Windows/player/PlayerInventory.h"
#include "Windows/player/CreativeMenu.h"
#include "Windows/player/RecipeInspectWindow.h"
#include "Windows/player/QuestBookWindow.h"
#include "Windows/player/ConsoleWindow.h"
#include "Windows/player/GameScenario.h"
#include "Panels/NeiPanel.h"

namespace UIDefaults {

void RegisterPlayerUI(UIManager& mgr, InventoryState& invState) {
    invState.slots.resize(40);
    mgr.SetPlayerInventory(&invState);

    auto& invWin = mgr.Register<PlayerInventory>(invState);
    invWin.SetDragManager(mgr.GetDragManager());
    invWin.SetBinder(&mgr.GetBinder());
    mgr.Register<CreativeMenu>(&mgr);
    mgr.Register<RecipeInspectWindow>(&mgr);
    mgr.Register<QuestBookWindow>(&mgr);
    mgr.Register<ConsoleWindow>(&mgr);
    mgr.Register<GameScenario>(&mgr);
    mgr.RegisterPanel<NeiPanel>(&mgr);

    mgr.GetBinder().LoadConfig("data/bindings.json");
}

IUIWindow* TryOpenBlockUI(UIManager& mgr, uint16_t blockId, const BlockPos& pos) {
    if (!BlockUIFactory::CanOpen(blockId))
        return nullptr;
    IUIWindow* win = BlockUIFactory::Create(blockId, pos, mgr);
    if (!win)
        return nullptr;
    mgr.OpenExclusive(win);
    return win;
}

}  // namespace UIDefaults
