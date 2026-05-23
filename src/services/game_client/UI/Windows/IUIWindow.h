#pragma once

#include <cstdint>
#include <string_view>

struct BlockPos;
struct InventoryState;

// ──────────────────────────────────────────────────────────────────────────────
// IUIWindow — base interface for all UI windows.
//
// Each window owns its own state, renders itself via ImGui, and can opt into
// input events and network updates.  Windows are registered in UIManager
// and never directly accessed by GameClient.
//
// Interface hierarchy:
//   IUIWindow
//    ├── InventoryWindow      (player inventory + hotbar)
//    ├── CreativeMenu         (creative item spawner)
//    └── BlockAttachedWindow  (windows attached to a world block)
//         ├── CraftingWindow
//         └── MachineWindow
// ──────────────────────────────────────────────────────────────────────────────
class IUIWindow {
public:
    virtual ~IUIWindow() = default;

    // ── Identification ────────────────────────────────────────────────────────
    virtual std::string_view Name() const = 0;

    // ── Rendering ─────────────────────────────────────────────────────────────
    // Called once per frame from UIManager::RenderAll (inside ImGui overlay).
    // playerInv is the shared player inventory (slots, selected slot, drag).
    // Each window decides what to draw based on its own IsOpen() state.
    virtual void Render(InventoryState* playerInv) = 0;

    // ── Input (early pass, before world interaction) ─────────────────────────
    // Return true if the event was consumed (stops propagation to other windows).
    virtual bool OnKeyEvent([[maybe_unused]] int key, [[maybe_unused]] int action, [[maybe_unused]] int mods) { return false; }
    virtual bool OnMouseClick([[maybe_unused]] int button, [[maybe_unused]] int action) { return false; }

    // ── State ─────────────────────────────────────────────────────────────────
    virtual bool IsOpen() const = 0;
    virtual void SetOpen(bool open) = 0;

    // ── Capture hints (used by UIManager) ────────────────────────────────────
    // If true, GameClient should skip world interaction (block break/place).
    virtual bool WantsMouseCapture() const { return IsOpen(); }
    virtual bool WantsKeyboardCapture() const { return IsOpen(); }

    // ── Network ───────────────────────────────────────────────────────────────
    // Called by UIManager::HandleNetwork when a message arrives.
    // msgType matches GatewayMsg constants (kInventoryUpdate, kBlockEntityUpdate…).
    virtual void OnNetworkUpdate([[maybe_unused]] uint8_t msgType, [[maybe_unused]] const void* data) {}

    // ── Factory hint ──────────────────────────────────────────────────────────
    // Returns true for windows tied to a world block (workbench, machine, chest…).
    virtual bool IsBlockAttached() const { return false; }
};
