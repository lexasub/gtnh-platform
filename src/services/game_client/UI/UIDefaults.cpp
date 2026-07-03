#include "UIDefaults.h"
#include "UIManager.h"
#include "BlockUIFactory.h"
#include "Windows/player/PlayerInventory.h"
#include "Windows/player/CreativeMenu.h"
#include "Windows/player/RecipeInspectWindow.h"
#include "Windows/player/QuestBookWindow.h"
#include "Panels/NeiPanel.h"
#include <GLFW/glfw3.h>

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

    // ── Default key bindings ──
    auto& binder = mgr.GetBinder();
    binder.Bind(GLFW_KEY_R,       "show_recipe");
    binder.Bind(GLFW_KEY_U,       "toggle_item_list");
    binder.Bind(GLFW_KEY_GRAVE_ACCENT,       "toggle_quest_book"); // Quest button is '`'
    binder.Bind(GLFW_KEY_ESCAPE,  "close_ui");
    for (int i = 0; i < 9; ++i)
        binder.Bind(GLFW_KEY_1 + i, "hotbar_" + std::to_string(i));
    binder.Bind(GLFW_KEY_0, "hotbar_9");
    // Scroll binding is set up in ActionHandler::Init
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
