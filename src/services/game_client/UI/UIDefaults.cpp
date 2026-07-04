#include "UIDefaults.h"
#include "UIManager.h"
#include "BlockUIFactory.h"
#include "Windows/player/PlayerInventory.h"
#include "Windows/player/CreativeMenu.h"
#include "Windows/player/RecipeInspectWindow.h"
#include "Windows/player/QuestBookWindow.h"
#include "Panels/NeiPanel.h"

namespace UIDefaults {

void RegisterPlayerUI(UIManager& mgr, InventoryState& invState) {
    invState.slots.resize(40);
    mgr.SetPlayerInventory(&invState);

    auto& invWin = mgr.Register<PlayerInventory>(invState);
    invWin.SetDragManager(mgr.GetDragManager());
    mgr.Register<CreativeMenu>(&mgr);
    mgr.Register<RecipeInspectWindow>();
    mgr.Register<QuestBookWindow>();
    mgr.RegisterPanel<NeiPanel>(&mgr);

    mgr.GetBinder().LoadConfig("data/bindings.json");
}

bool TryOpenBlockUI(UIManager& mgr, uint16_t blockId, const BlockPos& pos) {
    if (!BlockUIFactory::CanOpen(blockId))
        return false;
    IUIWindow* win = BlockUIFactory::Create(blockId, pos, mgr);
    if (!win)
        return false;
    mgr.OpenExclusive(win);
    return true;
}

}  // namespace UIDefaults
