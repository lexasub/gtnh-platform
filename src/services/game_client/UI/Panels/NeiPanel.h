#pragma once

#include "ISidePanel.h"
#include "Common/Types.h"
#include "UI/Components/ItemIndex.h"
#include <imgui.h>

class UIManager;
class MachineWindow;

class NeiPanel : public ISidePanel {
public:
    explicit NeiPanel(UIManager* uiMgr);

    std::string_view Name() const override { return "Recipes"; }
    void Render(InventoryState* playerInv) override;
    bool OnKeyEvent(int key, int action, int mods) override;

    bool IsVisible() const override { return visible_; }
    void SetVisible(bool visible) override {
        if (visible && !visible_) justOpened_ = true;
        visible_ = visible;
    }

protected:
    BlockPos activeMachinePos_;

private:
    bool visible_ = false;
    bool justOpened_ = false;
    UIManager* uiMgr_ = nullptr;
    int selectedRecipe_ = -1;
    char searchBuf_[128] = {};
    ItemIndex itemIndex_;


    void RenderMachineRecipes(MachineWindow* mw);
    void RenderAllRecipes();
};
