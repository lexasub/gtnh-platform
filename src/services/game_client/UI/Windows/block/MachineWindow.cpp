#include "MachineWindow.h"
#include "Components/SlotGrid.h"
#include "Components/PlayerInventoryGrid.h"
#include "Network/NetClient.h"
#include "RenderLib/Utils/TextureAtlas.h"
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
        case EnergyType::ROTATION: {
            int v = 140 + static_cast<int>(90 * ratio);
            return IM_COL32(v, 130, 220, 255);
        }
    }
    return IM_COL32(180, 180, 180, 255);
}

// Multiblock hatch type → display label (Protocol::HatchType values).
const char* HatchTypeName(uint8_t type) {
    switch (static_cast<Protocol::HatchType>(type)) {
        case Protocol::HatchType_ITEM_INPUT:   return "Item In";
        case Protocol::HatchType_ITEM_OUTPUT:  return "Item Out";
        case Protocol::HatchType_FLUID_INPUT:  return "Fluid In";
        case Protocol::HatchType_FLUID_OUTPUT: return "Fluid Out";
        case Protocol::HatchType_ENERGY:       return "Energy";
        case Protocol::HatchType_MUFFLER:      return "Muffler";
        default:                               return "Hatch";
    }
}

} // anonymous namespace

MachineWindow::MachineWindow(BlockPos pos, uint16_t machineType)
    : BlockAttachedWindow(pos)
    , machineType_(machineType) {}

void MachineWindow::SetOpen(bool open) {
    if (open && !open_) {
        // Slots are unloaded until the container_id=1 InventoryUpdate snapshot
        // arrives (Phase C); dataLoaded_ guards rendering of server slots.
        machineSlots_.clear();
        dataLoaded_ = false;
        if (netClient_ && player_id_ != 0) {
            netClient_->SendMachineOpenReq(player_id_, pos_.x, pos_.y, pos_.z);
        }
    }
    if (!open && open_) {
        if (netClient_ && player_id_ != 0) {
            netClient_->SendMachineCloseReq(player_id_, pos_.x, pos_.y, pos_.z);
        }
        dataLoaded_ = false;
    }
    open_ = open;
}

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

        if (dataLoaded_) {
            SlotGridComponent inGrid(machineSlots_);
            inGrid.SetStyle(style);
            inGrid.SetRange(0, inCount, inCount);
            inGrid.SetDragManager(dragMgr_);
            inGrid.SetBinder(binder_);
            inGrid.SetInventory(*playerInv);
            inGrid.SetAuthoritative(true);
            inGrid.SetContainerId(1);
            inGrid.Render();
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

        if (dataLoaded_) {
            SlotGridComponent outGrid(machineSlots_);
            outGrid.SetStyle(style);
            outGrid.SetRange(inCount, outCount, outCount);
            outGrid.SetDragManager(dragMgr_);
            outGrid.SetBinder(binder_);
            outGrid.SetInventory(*playerInv);
            outGrid.SetAuthoritative(true);
            outGrid.SetContainerId(1);
            outGrid.Render();
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

        // ── Multiblock hatches (task 3.1) ─────────────────────────────────
        if (!pendingHatches_.empty()) {
            ImGui::Separator();
            ImGui::Text("Hatches");
            for (const auto& hd : pendingHatches_) {
                ImGui::BulletText("%s @ (%d,%d,%d)", HatchTypeName(hd.type),
                                  hd.x, hd.y, hd.z);
                ImGui::Indent(12.0f);
                bool hasItem = false;
                for (const auto& item : hd.items) {
                    if (item.item_id == 0) continue;
                    hasItem = true;
                    ImGui::Text("item %u x%d", item.item_id, item.count);
                }
                if (!hasItem) {
                    ImGui::TextDisabled("(empty)");
                }
                ImGui::Unindent(12.0f);
            }
        }
    }

    ImGui::Separator();

    ImGui::PushID("machine_player_inv");
    RenderPlayerInventoryGrid(*playerInv, 0, static_cast<int>(playerInv->slots.size()),
                              9, playerInv->selectedSlot, false, dragMgr_, /*authoritative*/ true,
                              binder_);
    ImGui::PopID();

    RenderOutOfSyncWarning();

    // ── Cursor preview (server-owned hand stack) ────────────────────
    if (playerInv->cursor.item_id != 0) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImVec2 mouse = ImGui::GetIO().MousePos;
        auto uv = renderlib::TextureAtlas::GetItemUV(playerInv->cursor.item_id);
        dl->AddImage(
            renderlib::TextureAtlas::GetTextureHandle().idx,
            ImVec2(mouse.x + 4, mouse.y + 4),
            ImVec2(mouse.x + 40 - 4, mouse.y + 40 - 4),
            ImVec2(uv.u0, uv.v0),
            ImVec2(uv.u1, uv.v1));
        if (playerInv->cursor.count > 1) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d", playerInv->cursor.count);
            dl->AddText(ImVec2(mouse.x + 4, mouse.y + 4),
                        IM_COL32(255, 255, 255, 255), buf);
        }
    }

    ImGui::End();
}

void MachineWindow::OnNetworkUpdate(uint8_t msgType, const void* data) {
    if (msgType == GatewayMsg::kInventoryUpdate) {
        if (!data) return;
        flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(data), 8192);
        if (!v.VerifyBuffer<Protocol::InventoryUpdate>(nullptr)) return;
        auto* update = flatbuffers::GetRoot<Protocol::InventoryUpdate>(data);
        // container_id 0 = player, 1 = this machine; pos must match the open block
        if (update->container_id() != 1) return;
        auto* cp = update->container_pos();
        if (!cp || cp->x() != pos_.x || cp->y() != pos_.y || cp->z() != pos_.z) return;
        auto* cs = update->container_slots();
        int inCount = 3, outCount = 3;
        if (auto* info = MachineRegistry::instance()->Get(machineType_)) {
            inCount = info->slots_in;
            outCount = info->slots_out;
        }
        machineSlots_.assign(static_cast<size_t>(inCount + outCount), ItemStack{});
        size_t n = std::min(static_cast<size_t>(cs ? cs->size() : 0), machineSlots_.size());
        for (size_t i = 0; i < n; ++i) {
            auto* s = cs->Get(i);
            if (s) {
                machineSlots_[i] = {static_cast<uint16_t>(s->item_id()),
                                    static_cast<uint8_t>(s->count()),
                                    static_cast<uint16_t>(s->meta())};
            }
        }
        dataLoaded_ = true;
        return;
    }

    if (msgType == GatewayMsg::kBlockEntityUpdate) {
        // Diagnostic: log raw update
        flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(data), 8192);
        if (v.VerifyBuffer<Protocol::BlockEntityUpdate>(nullptr)) {
            auto* upd = flatbuffers::GetRoot<Protocol::BlockEntityUpdate>(data);
            auto* p = upd->pos();
            if (p && p->x() == pos_.x && p->y() == pos_.y && p->z() == pos_.z) {
                auto* out = upd->output_items();
                size_t outSz = out ? out->size() : 0;
                spdlog::info("[MachineWindow] BlockEntityUpdate at ({},{},{}): progress={} input={} output={}",
                             pos_.x, pos_.y, pos_.z, upd->progress(),
                             upd->input_items() ? upd->input_items()->size() : 0, outSz);
                if (out && out->size() > 0) {
                    auto* s = out->Get(0);
                    spdlog::info("[MachineWindow] First output item: id={} count={} meta={}",
                                 s->item_id(), s->count(), s->meta());
                }
            }
        }
    }

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

    // Multiblock hatches (task 3.1).
    pendingHatches_.clear();
    if (auto* hs = update->hatches()) {
        pendingHatches_.reserve(hs->size());
        for (flatbuffers::uoffset_t i = 0; i < hs->size(); ++i) {
            auto* h = hs->Get(i);
            if (!h) continue;
            HatchRenderData hd;
            if (h->pos()) {
                hd.x = h->pos()->x();
                hd.y = h->pos()->y();
                hd.z = h->pos()->z();
            }
            hd.type = static_cast<uint8_t>(h->hatch_type());
            if (auto* slots = h->slots()) {
                hd.items.reserve(slots->size());
                for (flatbuffers::uoffset_t j = 0; j < slots->size(); ++j) {
                    auto* ms = slots->Get(j);
                    if (!ms || !ms->item()) continue;
                    hd.items.push_back({
                        static_cast<uint16_t>(ms->item()->item_id()),
                        static_cast<uint8_t>(ms->item()->count()),
                        static_cast<uint16_t>(ms->item()->meta())});
                }
            }
            pendingHatches_.push_back(std::move(hd));
        }
    }

    hasPendingUpdate_ = true;
}
