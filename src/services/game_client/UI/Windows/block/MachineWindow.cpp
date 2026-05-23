#include "MachineWindow.h"
#include "Components/SlotGrid.h"
#include "Components/PlayerInventoryGrid.h"
#include "Network/NetClient.h"
#include "core_generated.h"
#include "recipe_generated.h"
#include <imgui.h>
#include <spdlog/spdlog.h>
#include <cstdio>
#include <cstdlib>
#include <algorithm>

namespace {

void renderEnergyBar(EnergyType et, uint32_t energy, uint32_t energyMax) {
    const char* label = MachineRegistry::EnergyLabel(et);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u / %u %s", energy, energyMax, label);
    float ratio = energyMax > 0 ? static_cast<float>(energy) / static_cast<float>(energyMax) : 0.0f;
    ImGui::ProgressBar(ratio, ImVec2(200, 16), buf);
}

} // anonymous namespace

MachineWindow::MachineWindow(BlockPos pos, uint16_t machineType)
    : BlockAttachedWindow(pos)
    , machineType_(machineType) {}

EnergyType MachineWindow::GetEnergyType() const {
    return energyType_;
}

void MachineWindow::SetEnergyType(EnergyType et) {
    energyType_ = et;
}

void MachineWindow::onMachineSlotAck(uint32_t /*x*/, uint32_t /*y*/, uint32_t /*z*/, uint8_t slotIdx, bool success) {
    if (!success) {
        lastErrorSlot_ = slotIdx;
        errorTimer_ = 500.0f; // 500ms error display
    } else {
        // On success: no-op (optimistic update already applied)
    }
}

void MachineWindow::Render(InventoryState* playerInv) {
    if (!open_) return;

    // Unique window ID per machine position (visible title stays "Machine")
    char title[64];
    std::snprintf(title, sizeof(title), "Machine###Machine_%d_%d_%d",
                  pos_.x, pos_.y, pos_.z);

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::Begin(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    // ── Title ─────────────────────────────────────────────────────────
    {
        const char* machineName = "Unknown";
        if (auto* info = MachineRegistry::instance()->Get(machineType_)) {
            machineName = info->name.c_str();
        }
        ImGui::Text("Machine %u (%s)", machineType_, machineName);
    }

    SlotStyle style;

    {
        // ── Data-driven path (no mechanism) ──────────────────────────────
        auto* info = MachineRegistry::instance()->Get(machineType_);
        int inCount = info ? info->slots_in : 3;
        int outCount = info ? info->slots_out : 3;

        for (int i = 0; i < inCount; ++i) {
            ImGui::PushID(100 + i);
            ItemStack item = (hasPendingUpdate_ && static_cast<size_t>(i) < pendingUpdate_.inputItems.size())
                ? pendingUpdate_.inputItems[i]
                : ItemStack{0, 0, 0};
            bool clicked = RenderSlot(item, false, ImGui::GetWindowDrawList(), style);
            if (clicked && netClient_) {
                if (playerInv->isDragging) {
                    spdlog::info("[MachineWindow] Drag-drop: srcSlot={} item={}x{} -> machine slot {} at ({},{},{})",
                                 playerInv->dragSourceSlot, playerInv->dragItem.item_id, playerInv->dragItem.count,
                                 i, pos_.x, pos_.y, pos_.z);
                    uint8_t srcSlot = (playerInv->dragSourceSlot >= 0)
                        ? static_cast<uint8_t>(playerInv->dragSourceSlot) : 255;
                    netClient_->SendSetMachineSlot(playerInv->player_id, pos_,
                        static_cast<uint16_t>(i),
                        playerInv->dragItem.item_id,
                        playerInv->dragItem.count,
                        playerInv->dragItem.meta,
                        srcSlot);
                    if (static_cast<size_t>(i) < pendingUpdate_.inputItems.size()) {
                        pendingUpdate_.inputItems[i] = playerInv->dragItem;
                    } else {
                        pendingUpdate_.inputItems.resize(i + 1);
                        pendingUpdate_.inputItems[i] = playerInv->dragItem;
                    }
                    hasPendingUpdate_ = true;
                    if (dragMgr_) {
                        dragMgr_->Reset();
                        dragMgr_->SyncTo(*playerInv);
                    }
                } else if (item.item_id != 0) {
                    spdlog::info("[MachineWindow] Pick up from machine slot {} at ({},{},{})", i, pos_.x, pos_.y, pos_.z);
                    // Pick up item from machine input slot into cursor
                    if (dragMgr_) {
                        dragMgr_->StartExternalDrag(100 + i, item);
                        dragMgr_->SetMachineDragContext(pos_, i);
                        dragMgr_->SyncTo(*playerInv);
                    } else {
                        playerInv->dragItem = item;
                        playerInv->isDragging = true;
                        playerInv->dragSourceSlot = -(100 + i);
                    }
                    netClient_->SendSetMachineSlot(playerInv->player_id, pos_,
                        static_cast<uint16_t>(i), 0, 0, 0);
                    if (static_cast<size_t>(i) < pendingUpdate_.inputItems.size()) {
                        pendingUpdate_.inputItems[i] = ItemStack{0, 0, 0};
                    }
                    hasPendingUpdate_ = true;
                } else {
                    spdlog::info("[MachineWindow] Clicked empty slot {} at ({},{},{}) isDragging={}",
                                 i, pos_.x, pos_.y, pos_.z, playerInv->isDragging);
                }
            }
            ImGui::SameLine();
            ImGui::PopID();
        }

        float prog = hasPendingUpdate_ ? pendingUpdate_.progress : 0.0f;

        // Recipe completed green flash overlay
        if (recipeDoneFlash_ > 0.0f) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImVec2 p1 = ImVec2(p0.x + 80, p0.y + 24);
            dl->AddRectFilled(p0, p1, IM_COL32(0, 255, 0, static_cast<int>(80 * recipeDoneFlash_)));
            recipeDoneFlash_ -= ImGui::GetIO().DeltaTime;
        }

        ImGui::ProgressBar(prog, ImVec2(80, 24), "");
        ImGui::SameLine();

        for (int i = 0; i < outCount; ++i) {
            ImGui::PushID(200 + i);
            ItemStack item = (hasPendingUpdate_ && static_cast<size_t>(i) < pendingUpdate_.outputItems.size())
                ? pendingUpdate_.outputItems[i]
                : ItemStack{0, 0, 0};
            bool clicked = RenderSlot(item, false, ImGui::GetWindowDrawList(), style);
            if (clicked && netClient_) {
                if (playerInv->isDragging) {
                    spdlog::info("[MachineWindow] Drag-drop output: srcSlot={} item={}x{} -> out slot {} at ({},{},{})",
                                 playerInv->dragSourceSlot, playerInv->dragItem.item_id, playerInv->dragItem.count,
                                 i, pos_.x, pos_.y, pos_.z);
                    uint8_t srcSlot = (playerInv->dragSourceSlot >= 0)
                        ? static_cast<uint8_t>(playerInv->dragSourceSlot) : 255;
                    netClient_->SendSetMachineSlot(playerInv->player_id, pos_,
                        static_cast<uint16_t>(inCount + i),
                        playerInv->dragItem.item_id,
                        playerInv->dragItem.count,
                        playerInv->dragItem.meta,
                        srcSlot);
                    if (static_cast<size_t>(i) < pendingUpdate_.outputItems.size()) {
                        pendingUpdate_.outputItems[i] = playerInv->dragItem;
                    } else {
                        pendingUpdate_.outputItems.resize(i + 1);
                        pendingUpdate_.outputItems[i] = playerInv->dragItem;
                    }
                    hasPendingUpdate_ = true;
                    if (dragMgr_) {
                        dragMgr_->Reset();
                        dragMgr_->SyncTo(*playerInv);
                    }
                } else if (item.item_id != 0) {
                    spdlog::info("[MachineWindow] Pick up from output slot {} at ({},{},{})", i, pos_.x, pos_.y, pos_.z);
                    // Pick up output item into cursor
                    if (dragMgr_) {
                        dragMgr_->StartExternalDrag(200 + i, item);
                        dragMgr_->SetMachineDragContext(pos_, inCount + i);
                        dragMgr_->SyncTo(*playerInv);
                    } else {
                        playerInv->dragItem = item;
                        playerInv->isDragging = true;
                        playerInv->dragSourceSlot = -(100 + inCount + i);
                    }
                    netClient_->SendSetMachineSlot(playerInv->player_id, pos_,
                        static_cast<uint16_t>(inCount + i), 0, 0, 0);
                    if (static_cast<size_t>(i) < pendingUpdate_.outputItems.size()) {
                        pendingUpdate_.outputItems[i] = ItemStack{0, 0, 0};
                    }
                    hasPendingUpdate_ = true;
                }
            }
            ImGui::SameLine();
            ImGui::PopID();
        }

        ImGui::Separator();

        EnergyType energyType = hasPendingUpdate_
            ? static_cast<EnergyType>(pendingUpdate_.energyType)
            : GetEnergyType();
        uint32_t energyMax = hasPendingUpdate_ && pendingUpdate_.energyCapacity > 0
            ? pendingUpdate_.energyCapacity
            : (info ? (static_cast<uint32_t>(info->tier * 10000) > 0 ? static_cast<uint32_t>(info->tier * 10000) : 10000) : 10000);
        uint32_t energyVal = hasPendingUpdate_ ? pendingUpdate_.energy : 0;
        renderEnergyBar(energyType, energyVal, energyMax);
    }

    ImGui::Separator();

    // ── Update slot error states ──────────────────────────────────────────
    for (auto& error : slotErrors_) {
        error.flashTimer -= 0.016f;
    }
    slotErrors_.erase(std::remove_if(slotErrors_.begin(), slotErrors_.end(),
        [](const SlotErrorState& e) { return e.flashTimer <= 0.0f; }),
        slotErrors_.end());

    // ── Drag preview (same pattern as ClientCraftingWindow) ───────────────
    if (playerInv->isDragging) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        uint32_t itemColor = IM_COL32(
            playerInv->dragItem.item_id * 50 % 256,
            playerInv->dragItem.item_id * 80 % 256,
            playerInv->dragItem.item_id * 30 % 256, 255);
        ImVec2 mousePos = ImGui::GetIO().MousePos;
        dl->AddRectFilled(ImVec2(mousePos.x + 4, mousePos.y + 4),
                          ImVec2(mousePos.x + 40 - 4, mousePos.y + 40 - 4),
                          itemColor, 2.0f);
        if (playerInv->dragItem.count > 1) {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%d", playerInv->dragItem.count);
            dl->AddText(ImVec2(mousePos.x + 4, mousePos.y + 4),
                        IM_COL32(255, 255, 255, 255), buf);
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            // Cancel drag - restore item to source slot
            int absSlot = std::abs(playerInv->dragSourceSlot);
            if (absSlot >= 100) {
                // Drag originated from machine (input/output slot)
                int inCount = 3;
                if (auto* info = MachineRegistry::instance()->Get(machineType_)) {
                    inCount = info->slots_in;
                }
                int machineSlot = (absSlot >= 200) ? inCount + (absSlot - 200) : (absSlot - 100);
                netClient_->SendSetMachineSlot(playerInv->player_id, pos_,
                    static_cast<uint16_t>(machineSlot),
                    playerInv->dragItem.item_id,
                    playerInv->dragItem.count,
                    playerInv->dragItem.meta,
                    255);
            } else {
                // Drag originated from player inventory
                netClient_->SendSetMachineSlot(playerInv->player_id, pos_,
                    static_cast<uint16_t>(playerInv->dragSourceSlot), 0, 0, 0);
            }
            if (dragMgr_) {
                dragMgr_->Reset();
                dragMgr_->SyncTo(*playerInv);
            } else {
                playerInv->isDragging = false;
                playerInv->dragItem = ItemStack{};
                playerInv->dragSourceSlot = -1;
            }
        }
    }

    ImGui::PushID("player_inv");
    {
        int clicked = RenderPlayerInventoryGrid(*playerInv, 0, static_cast<int>(playerInv->slots.size()), 9, -1, false, dragMgr_);
        if (clicked >= 0) {
            spdlog::info("MachineWindow: clicked slot={} dragging={}", clicked, playerInv->isDragging);
        }
    }
    ImGui::PopID();

    ImGui::End();
}

void MachineWindow::OnNetworkUpdate(uint8_t msgType, const void* data) {
    if (msgType == GatewayMsg::kRecipeCompleted) {
        // Recipe completed notification — flash the progress bar
        flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(data), 8192);
        if (!v.VerifyBuffer<Protocol::RecipeCompleted>(nullptr)) return;
        auto* rc = flatbuffers::GetRoot<Protocol::RecipeCompleted>(data);
        auto* p = rc->pos();
        if (!p || p->x() != pos_.x || p->y() != pos_.y || p->z() != pos_.z) return;
        recipeDoneFlash_ = 2.0f; // 2 seconds green flash
        return;
    }

    if (msgType != GatewayMsg::kBlockEntityUpdate) {
        return;
    }

    if (!data) {
        return;
    }

    // ── Parse FlatBuffer BlockEntityUpdate ────────────────────────────
    flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(data), 8192);
    if (!v.VerifyBuffer<Protocol::BlockEntityUpdate>(nullptr)) {
        spdlog::warn("MachineWindow: invalid BlockEntityUpdate");
        return;
    }

    auto* update = flatbuffers::GetRoot<Protocol::BlockEntityUpdate>(data);

    // Only accept updates for this machine's position
    auto* updatePos = update->pos();
    if (!updatePos || updatePos->x() != pos_.x || updatePos->y() != pos_.y || updatePos->z() != pos_.z) {
        return;
    }

    pendingUpdate_.energy = update->energy();
    pendingUpdate_.progress = update->progress();
    pendingUpdate_.energyCapacity = update->energy_capacity();
    pendingUpdate_.energyType = static_cast<EnergyType>(update->energy_type());

    pendingUpdate_.inputItems.clear();
    if (auto* inItems = update->input_items()) {
        pendingUpdate_.inputItems.reserve(inItems->size());
        for (flatbuffers::uoffset_t i = 0; i < inItems->size(); ++i) {
            auto* s = inItems->Get(i);
            pendingUpdate_.inputItems.push_back({
                static_cast<uint16_t>(s ? s->item_id() : 0),
                static_cast<uint8_t>(s ? s->count() : 0),
                static_cast<uint16_t>(s ? s->meta() : 0)});
        }
    }

    pendingUpdate_.outputItems.clear();
    if (auto* outItems = update->output_items()) {
        pendingUpdate_.outputItems.reserve(outItems->size());
        for (flatbuffers::uoffset_t i = 0; i < outItems->size(); ++i) {
            auto* s = outItems->Get(i);
            pendingUpdate_.outputItems.push_back({
                static_cast<uint16_t>(s ? s->item_id() : 0),
                static_cast<uint8_t>(s ? s->count() : 0),
                static_cast<uint16_t>(s ? s->meta() : 0)});
        }
    }

    hasPendingUpdate_ = true;
}
