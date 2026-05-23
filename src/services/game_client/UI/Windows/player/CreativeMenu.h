#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>

#include "../IUIWindow.h"

class UIManager;

// ── CreativeMenu — implements IUIWindow ─────────────────────────────────────
// Opens with TAB. Lists all items from items.csv + MachineRegistry.
class CreativeMenu : public IUIWindow {
public:
    CreativeMenu(UIManager* mgr);

    std::string_view Name() const override { return "CreativeMenu"; }

    void Render(InventoryState* playerInv) override;
    bool OnKeyEvent(int key, int action, int mods) override;

    bool IsOpen() const override { return open_; }
    void SetOpen(bool open) override { open_ = open; }

    bool WantsMouseCapture() const override { return open_; }
    bool WantsKeyboardCapture() const override { return open_; }

private:
    struct Item {
        uint16_t id;
        std::string name;
        bool isMachine = false; // from MachineRegistry (not in items.csv)
    };
    std::vector<Item> items_;
    void rebuildItemList();

    bool open_ = false;
    char searchBuf_[64] = "";
    uint16_t selectedItem_ = 0;
    int count_ = 64;
    UIManager* uiMgr_ = nullptr;
};
