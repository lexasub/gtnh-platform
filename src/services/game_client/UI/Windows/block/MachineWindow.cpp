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

// ── Arrow-shaped progress (furnace / macerator / compressor) ──────────
void DrawArrowProgress(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float t, ImU32 color) {
    float w = p1.x - p0.x;
    float h = p1.y - p0.y;

    dl->AddRectFilled(p0, p1, IM_COL32(40, 40, 40, 255), 4.0f);

    if (t > 0.0f) {
        float fillW = w * std::min(t, 1.0f);
        ImVec2 fillEnd(p0.x + fillW, p1.y);
        dl->AddRectFilled(p0, fillEnd, color, 4.0f);

        // Arrow head triangle on the right side of fill
        if (fillW >= 12.0f) {
            float midY = (p0.y + p1.y) * 0.5f;
            float arrowW = 10.0f;
            float arrowH = h * 0.6f;
            ImVec2 apex(p0.x + fillW, midY);
            dl->AddTriangleFilled(
                ImVec2(apex.x - arrowW, apex.y - arrowH),
                ImVec2(apex.x - arrowW, apex.y + arrowH),
                apex, IM_COL32(255, 255, 255, 80));
        }
    }

    dl->AddRect(p0, p1, IM_COL32(100, 100, 100, 255), 4.0f);
}

// ── Spinner progress (centrifuge / mixer) ─────────────────────────────
void DrawSpinnerProgress(ImDrawList* dl, ImVec2 center, float radius,
                         float t, ImU32 color) {
    dl->AddCircleFilled(center, radius, IM_COL32(40, 40, 40, 255), 24);

    int numSegs = 8;
    float arcAngle = ImGui::GetTime() * 4.0f;
    float segArc = (3.14159265f * 2.0f) / numSegs;
    int showSegs = static_cast<int>(t * numSegs);

    for (int i = 0; i < numSegs; ++i) {
        float a0 = arcAngle + i * segArc;
        float a1 = a0 + segArc * 0.7f;
        ImU32 segColor = (i < showSegs) ? color : IM_COL32(60, 60, 60, 255);
        dl->PathArcTo(center, radius - 2.0f, a0, a1, 6);
        dl->PathStroke(segColor, false, 4.0f);
    }

    dl->AddCircleFilled(center, 3.0f, IM_COL32(150, 150, 150, 255), 8);
    dl->AddCircle(center, radius, IM_COL32(100, 100, 100, 255), 24, 1.5f);
}

// ── Flame progress (boiler / generator) ───────────────────────────────
void DrawFlameProgress(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float t, ImU32 color) {
    float w = p1.x - p0.x;
    float h = p1.y - p0.y;

    dl->AddRectFilled(p0, p1, IM_COL32(40, 40, 40, 255), 4.0f);

    if (t > 0.0f) {
        float fillH = h * std::min(t, 1.0f);
        float yTop = p1.y - fillH;

        dl->AddRectFilled(ImVec2(p0.x, yTop), p1, color, 4.0f);

        // Animated wavy flame top edge
        float wave = std::sin(ImGui::GetTime() * 6.0f + p0.x * 0.1f) * 3.0f;
        ImVec2 flameTop(p0.x + w * 0.5f, yTop + wave);

        dl->AddCircleFilled(flameTop, 6.0f,
            IM_COL32(255, 200, 50, static_cast<int>(80 * t)), 12);
    }

    dl->AddRect(p0, p1, IM_COL32(100, 100, 100, 255), 4.0f);
}

ImU32 EnergyBarColor(EnergyType et, float ratio) {
    switch (et) {
        case EnergyType::ELECTRICITY: {
            int v = 180 + static_cast<int>(75 * ratio);
            return IM_COL32(v, v, 50, 255);
        }
        case EnergyType::HEAT: {
            int r = 255;
            int g = static_cast<int>(60 + 150 * ratio);
            return IM_COL32(r, g, 0, 255);
        }
        case EnergyType::STEAM: {
            int b = 255;
            int g = static_cast<int>(140 + 80 * ratio);
            return IM_COL32(80, g, b, 255);
        }
    }
    return IM_COL32(180, 180, 180, 255);
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

MachineWindow::ProgressStyle MachineWindow::ResolveProgressStyle(const MachineInfo* info) {
    if (!info) return ProgressStyle::GENERIC;
    const auto& cls = info->machine_class;
    if (cls == "furnace" || cls == "macerator" || cls == "compressor" ||
        cls == "extractor" || cls == "alloy_smelter") {
        return ProgressStyle::ARROW;
    }
    if (cls == "mixer" || cls == "electrolyser" || cls == "chemical_reactor" ||
        cls == "crystallizer" || cls == "assembler") {
        return ProgressStyle::SPINNER;
    }
    if (cls == "boiler" || cls == "generator") {
        return ProgressStyle::FLAME;
    }
    return ProgressStyle::GENERIC;
}

void MachineWindow::onMachineSlotAck(uint32_t /*x*/, uint32_t /*y*/, uint32_t /*z*/, uint8_t slotIdx, bool success) {
    if (!success) {
        lastErrorSlot_ = slotIdx;
        errorTimer_ = 500.0f; // 500ms error display
    } else {
        // On success: no-op (optimistic update already applied)
    }
    // Send to DragManager for unified drag state management
    if (dragMgr_) {
        dragMgr_->OnMachineSlotAck(slotIdx, success);
    }
}

// ── Progress rendering dispatcher ──────────────────────────────────────────
void MachineWindow::RenderProgress(const MachineInfo* info, float prog) {
    if (!info) {
        ImGui::ProgressBar(prog, ImVec2(80, 24), "");
        return;
    }

    if (!styleCached_) {
        cachedStyle_ = ResolveProgressStyle(info);
        styleCached_ = true;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();

    switch (cachedStyle_) {
        case ProgressStyle::ARROW: {
            ImVec2 p1(p0.x + 80, p0.y + 24);
            DrawArrowProgress(dl, p0, p1, prog, IM_COL32(255, 160, 40, 255));
            ImGui::Dummy(ImVec2(80, 24));
            break;
        }
        case ProgressStyle::SPINNER: {
            ImVec2 center(p0.x + 40, p0.y + 20);
            DrawSpinnerProgress(dl, center, 18.0f, prog, IM_COL32(80, 200, 255, 255));
            ImGui::Dummy(ImVec2(80, 40));
            break;
        }
        case ProgressStyle::FLAME: {
            ImVec2 p1(p0.x + 40, p0.y + 28);
            DrawFlameProgress(dl, p0, p1, prog, IM_COL32(255, 100, 0, 255));
            ImGui::Dummy(ImVec2(40, 28));
            break;
        }
        default: {
            ImGui::ProgressBar(prog, ImVec2(80, 24), "");
            break;
        }
    }
}

// ── Energy bar with color-coding ───────────────────────────────────────────
void MachineWindow::RenderEnergyBarImpl(EnergyType et, uint32_t energy, uint32_t energyMax,
                                         float heatRatio, uint64_t mbId) {
    const char* label = MachineRegistry::EnergyLabel(et);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s: %u / %u", label, energy, energyMax);
    float ratio = energyMax > 0 ? static_cast<float>(energy) / static_cast<float>(energyMax) : 0.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float w = 200.0f;
    float h = 18.0f;
    ImVec2 p1(p0.x + w, p0.y + h);

    dl->AddRectFilled(p0, p1, IM_COL32(40, 40, 40, 255), 3.0f);

    if (ratio > 0.0f) {
        ImVec2 fillEnd(p0.x + w * std::min(ratio, 1.0f), p1.y);
        ImU32 barColor = EnergyBarColor(et, ratio);

        // Overheat tints for multiblock machines only
        if (mbId > 0) {
            if (heatRatio >= 1.0f) {
                barColor = IM_COL32(255, 40, 40, 255);  // Red: critical
            } else if (heatRatio >= 0.9f) {
                barColor = IM_COL32(255, 200, 0, 255);  // Yellow: warning
            }
        }

        dl->AddRectFilled(p0, fillEnd, barColor, 3.0f);
    }

    dl->AddRect(p0, p1, IM_COL32(80, 80, 80, 255), 3.0f);

    ImVec2 textSize = ImGui::CalcTextSize(buf);
    ImVec2 textPos(p0.x + (w - textSize.x) * 0.5f, p0.y + (h - textSize.y) * 0.5f);
    dl->AddText(textPos, IM_COL32(220, 220, 220, 255), buf);

    ImGui::Dummy(ImVec2(w, h + 2.0f));
}

// ── Out-of-sync warning ────────────────────────────────────────────────────
void MachineWindow::RenderOutOfSyncWarning() {
    if (framesSinceUpdate_ < kOutOfSyncFrames) return;

    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 180, 0, 255));
    ImGui::Text("⚠ Connection to machine lost — state may be stale");
    ImGui::PopStyleColor();
}

void MachineWindow::Render(InventoryState* playerInv) {
    if (!open_) return;

    ++framesSinceUpdate_;
    //TODO refactor hell
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

        RenderProgress(info, prog);
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
        RenderEnergyBarImpl(energyType, energyVal, energyMax,
                            hasPendingUpdate_ ? pendingUpdate_.heatRatio : 0.0f,
                            hasPendingUpdate_ ? pendingUpdate_.mbId : 0);
    }

    ImGui::Separator();

    // ── Update slot error states ──────────────────────────────────────────
    for (auto& error : slotErrors_) {
        error.flashTimer -= 0.016f;
    }
    slotErrors_.erase(std::remove_if(slotErrors_.begin(), slotErrors_.end(),
        [](const SlotErrorState& e) { return e.flashTimer <= 0.0f; }),
        slotErrors_.end());

    // ── Drag preview через DragManager ──────────────────────────────────
    if (dragMgr_ && dragMgr_->IsDragging()) {
        dragMgr_->RenderPreview(style);

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            // Cancel drag — restore item to source
            if (dragMgr_->HasMachineDragContext()) {
                // Drag originated from a machine slot → restore via server
                BlockPos mp = dragMgr_->GetMachineDragPos();
                int ms = dragMgr_->GetMachineDragSlotIdx();
                const ItemStack& held = dragMgr_->GetHeldItem();
                netClient_->SendSetMachineSlot(playerInv->player_id, mp,
                    static_cast<uint16_t>(ms),
                    held.item_id, held.count, held.meta, 255);
                // Restore pending update
                if (ms < static_cast<int>(pendingUpdate_.inputItems.size())) {
                    pendingUpdate_.inputItems[ms] = held;
                }
                hasPendingUpdate_ = true;
            }
            // CancelDrag returns item to inventory slot if source was inventory
            dragMgr_->CancelDrag(playerInv->slots);
            dragMgr_->SyncTo(*playerInv);
        }
    } else if (!dragMgr_ && playerInv->isDragging) {
        // Fallback: manual preview without DragManager
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
            }
            playerInv->isDragging = false;
            playerInv->dragItem = ItemStack{};
            playerInv->dragSourceSlot = -1;
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

    RenderOutOfSyncWarning();

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
    pendingUpdate_.heatRatio = update->temperature();
    pendingUpdate_.mbId = update->mb_id();
    framesSinceUpdate_ = 0;

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
