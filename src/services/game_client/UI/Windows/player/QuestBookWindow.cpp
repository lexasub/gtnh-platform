#include "QuestBookWindow.h"
#include "Crafting/ClientItemRegistry.h"
#include "Network/NetClient.h"
#include "RenderLib/Utils/TextureAtlas.h"
#include "UI/UIManager.h"
#include "quest_lib/QuestData.h"
#include "quest_lib/QuestGraph.h"
#include "quest_generated.h"
#include <common/ItemId.h>
#include <imgui.h>
#include <spdlog/spdlog.h>
#include <string>

QuestBookWindow::QuestBookWindow(UIManager *mgr) : uiMgr_(mgr) {
    loadQuestData();
}

namespace {

std::string itemLabel(uint16_t itemId, const std::string &spec) {
    if (const ItemRegistry::ItemInfo *item = ItemRegistry::GetItem(itemId)) {
        const std::string &id = spec.empty() ? std::to_string(itemId) : spec;
        return item->name + " (" + id + ")";
    }
    if (!spec.empty())
        return spec;
    return "item " + std::to_string(itemId);
}

} // namespace

void QuestBookWindow::SetOpen(bool open) {
    open_ = open;
}

void QuestBookWindow::SetEra(int eraIndex) {
    if (!eraData_.empty()) {
        if (eraIndex < 0) eraIndex = 0;
        if (eraIndex >= static_cast<int>(eraData_.size()))
            eraIndex = static_cast<int>(eraData_.size()) - 1;
    }
    selectedEra_ = eraIndex;
    selectedSection_ = 0;
    selectedQuestId_ = 0;
    selectedChoice_ = 0;
    newlyAvailableEra_ = -1;
}

void QuestBookWindow::loadQuestData() {
    quest::QuestData& qd = questData_;
    std::string dataDir = DATA_DIR;
    if (!qd.LoadCSV(dataDir + "/quests/quests.csv")) {
        spdlog::error("[Quest] Failed to load quests.csv");
        return;
    }
    qd.LoadGraph(dataDir + "/quests/quest_graph.json");
    qd.LoadRequirementsJSON(dataDir + "/quests/quest_requirements.json");
    qd.LoadRewardsJSON(dataDir + "/quests/quest_rewards.json");

    {
        std::unordered_map<uint32_t, std::vector<uint32_t>> prereqs;
        for (const auto& qdRef : qd.AllQuests())
            prereqs[qdRef.id] = qd.GetPrerequisites(qdRef.id);
        questGraph_.Init(qd.Graph(), prereqs);
    }

    eraData_.clear();
    quests_.clear();
    for (const auto& qdRef : qd.AllQuests()) {
        QuestEntry e;
        e.id = qdRef.id;
        e.title = qdRef.title;
        e.description = qdRef.description;
        e.section = qdRef.section;
        e.rewardItemId = qdRef.rewardItemId;
        e.rewardCount = qdRef.rewardCount;
        e.costItemId = qdRef.costItemId;
        e.costCount = qdRef.costCount;
        e.cooldownSecs = qdRef.cooldownSecs;
        e.isExchange = (qdRef.detectType == quest::DetectionType::EXCHANGE);
        e.isInventory = (qdRef.detectType == quest::DetectionType::INVENTORY);
        e.targetItemId = e.isInventory ? ItemId::pack(qdRef.detectTarget) : 0;
        e.targetCount = qdRef.targetCount;
        e.autoComplete = qdRef.autoComplete;
        e.requirements = qdRef.requirements;
        if (const auto* rw = qd.GetReward(qdRef.id); rw) {
            e.reward = *rw;
            if (!rw->rewards.empty() && rw->rewards[0].type == quest::RewardType::ITEM) {
                e.rewardItemId = ItemId::pack(rw->rewards[0].item);
                e.rewardCount = static_cast<uint8_t>(rw->rewards[0].count);
            }
        }
        e.status = 0;
        e.progress = 0;
        quests_.push_back(e);
    }

    eraData_ = qd.BuildEraStructure();
    eraCompleted_.assign(eraData_.size(), false);
    newlyAvailableEra_ = -1;
    dataLoaded_ = true;
    spdlog::info("[Quest] Loaded {} quests across {} eras", quests_.size(), eraData_.size());
}

void QuestBookWindow::Render(InventoryState* playerInv) {
    if (!open_) return;
    if (!dataLoaded_) {
        loadQuestData();
        if (!dataLoaded_) return;
    }

    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Quest Book", &open_, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    renderEraTabs();
    ImGui::Separator();

    float leftWidth = 180.0f;
    float midWidth = 220.0f;

    if (ImGui::BeginChild("leftPanel", ImVec2(leftWidth, 0), ImGuiChildFlags_Borders)) {
        renderSectionPanel();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    if (ImGui::BeginChild("midPanel", ImVec2(midWidth, 0), ImGuiChildFlags_Borders)) {
        renderQuestList();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    if (ImGui::BeginChild("rightPanel", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        renderQuestDetail(playerInv);
    }
    ImGui::EndChild();

    ImGui::End();
}

void QuestBookWindow::renderEraTabs() {
    if (eraData_.empty()) return;
    if (newlyAvailableEra_ >= 0 && static_cast<size_t>(newlyAvailableEra_) < eraData_.size()) {
        selectedEra_ = newlyAvailableEra_;
        selectedSection_ = 0;
        selectedQuestId_ = 0;
        selectedChoice_ = 0;
        newlyAvailableEra_ = -1;
    }
    if (ImGui::BeginTabBar("eraTabs")) {
        for (size_t i = 0; i < eraData_.size(); ++i) {
            std::string label = eraData_[i].label;
            if (i < eraCompleted_.size() && eraCompleted_[i]) label += " ✓";
            if (static_cast<int>(i) == newlyAvailableEra_)
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 200, 50, 255));
            if (ImGui::TabItemButton(label.c_str(),
                i == static_cast<size_t>(selectedEra_) ? ImGuiTabItemFlags_SetSelected : 0)) {
                selectedEra_ = static_cast<int>(i);
                selectedSection_ = 0;
                selectedQuestId_ = 0;
                selectedChoice_ = 0;
            }
            if (static_cast<int>(i) == newlyAvailableEra_)
                ImGui::PopStyleColor();
        }
        ImGui::EndTabBar();
    }
}

void QuestBookWindow::renderSectionPanel() {
    if (selectedEra_ < 0 || static_cast<size_t>(selectedEra_) >= eraData_.size()) return;
    const auto& era = eraData_[selectedEra_];
    for (size_t i = 0; i < era.sections.size(); ++i) {
        bool isSelected = (static_cast<size_t>(selectedSection_) == i);
        if (ImGui::Selectable(era.sections[i].label.c_str(), isSelected)) {
            selectedSection_ = static_cast<int>(i);
            selectedQuestId_ = 0;
            selectedChoice_ = 0;
        }
    }
}

void QuestBookWindow::renderQuestList() {
    if (selectedEra_ < 0 || static_cast<size_t>(selectedEra_) >= eraData_.size()) return;
    const auto& era = eraData_[selectedEra_];
    if (selectedSection_ < 0 || static_cast<size_t>(selectedSection_) >= era.sections.size()) return;
    const auto& section = era.sections[selectedSection_];

    for (uint32_t qid : section.questIds) {
        auto it = std::find_if(quests_.begin(), quests_.end(),
            [qid](const QuestEntry& e) { return e.id == qid; });
        if (it == quests_.end()) continue;

        bool isSelected = (it->id == selectedQuestId_);
        std::string label = statusLabel(it->status) + std::string(" ") + it->title;
        ImU32 col = statusColor(it->status);
        ImGui::PushStyleColor(ImGuiCol_Text, col);

        if (ImGui::Selectable(label.c_str(), isSelected)) {
            selectedQuestId_ = it->id;
            selectedChoice_ = 0;
        }
        ImGui::PopStyleColor();
    }
}

void QuestBookWindow::renderQuestDetail(const InventoryState* playerInv) {
    uint64_t playerId = playerInv ? playerInv->player_id : 0;
    if (selectedQuestId_ == 0) {
        ImGui::TextWrapped("Select a quest to view details.");
        return;
    }

    auto it = std::find_if(quests_.begin(), quests_.end(),
        [this](const QuestEntry& e) { return e.id == selectedQuestId_; });
    if (it == quests_.end()) return;

    ImGui::PushStyleColor(ImGuiCol_Text, statusColor(it->status));
    ImGui::Text("%s", it->title.c_str());
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::TextWrapped("%s", it->description.c_str());
    ImGui::Dummy(ImVec2(0, 8));

    ImGui::Text("Status: %s", statusLabel(it->status));
    if (it->status == 2) {
        ImGui::ProgressBar(static_cast<float>(it->progress) / 100.0f, ImVec2(-1, 0),
            std::to_string(it->progress).append("%").c_str());
    }

    if (!it->reward.rewards.empty() && !it->reward.choiceOf.empty()) {
        // XOR invariant should make this unreachable; keep both visible.
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Text("Rewards:");
        for (const auto& r : it->reward.rewards) renderRewardEntry(r);
        ImGui::Text("Or choose:");
        for (const auto& r : it->reward.choiceOf) renderRewardEntry(r);
    } else if (!it->reward.rewards.empty()) {
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Text("Rewards:");
        for (const auto& r : it->reward.rewards) renderRewardEntry(r);
    } else if (!it->reward.choiceOf.empty()) {
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Text("Reward (choose one):");
        size_t choice = 0;
        for (const auto& r : it->reward.choiceOf) {
            ImGui::RadioButton("##choice", &selectedChoice_, static_cast<int>(choice));
            ImGui::SameLine();
            renderRewardEntry(r);
            ++choice;
        }
        if (selectedChoice_ >= static_cast<int>(it->reward.choiceOf.size()))
            selectedChoice_ = 0;
        if (!it->reward.choiceOf.empty()) {
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::Indent();
            renderRewardEntry(it->reward.choiceOf[static_cast<size_t>(selectedChoice_)]);
            ImGui::Unindent();
        }
    } else if (it->rewardItemId > 0 && !it->isExchange) {
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Text("Reward: %s x %u", itemLabel(it->rewardItemId, "").c_str(),
                    it->rewardCount);
    }

    // Structured objectives from quest_requirements.json. Each requirement
    // renders an icon row (item + kind + count). INVENTORY needs are colored
    // by whether the player currently holds the item.
    if (!it->requirements.empty()) {
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Text("Objectives:");
        for (const auto& req : it->requirements)
            renderRequirementRow(req, playerInv);
    }

    if (it->isExchange) {
        ImGui::Dummy(ImVec2(0, 4));
        if (it->costItemId > 0) {
            ImGui::Text("Cost: %s x %u", itemLabel(it->costItemId, "").c_str(),
                        it->costCount);
        }
        ImGui::Text("Reward: %s x %u", itemLabel(it->rewardItemId, "").c_str(),
                    it->rewardCount);
        if (it->cooldownSecs > 0) {
            ImGui::Text("Cooldown: %us", it->cooldownSecs);
        }

        // Pull the server-side cooldown once per quest detail open; the
        // response arrives async on kQuestExchangeCooldown.
        if (cooldownQueriedQuestId_ != it->id) {
            cooldownQueriedQuestId_ = it->id;
            cooldownRemainingSecs_ = 0;
            exchangeMessage_.clear();
            onCooldownQuery(playerId);
        }

        ImGui::Dummy(ImVec2(0, 8));
        ImGui::PushID(static_cast<int>(it->id));
        bool onCooldown = cooldownRemainingSecs_ > 0;
        bool lacksCost = playerId != 0 && it->costItemId > 0 &&
                         countItem(playerInv, it->costItemId) < it->costCount;
        if (onCooldown || lacksCost) {
            if (onCooldown) {
                ImGui::Text("Exchange again in %us", cooldownRemainingSecs_);
            } else if (lacksCost) {
                ImGui::Text("Need %u more to exchange", it->costCount - countItem(playerInv, it->costItemId));
            }
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Exchange", ImVec2(120, 0))) {
            onExchangeClicked(playerId);
        }
        if (onCooldown || lacksCost) {
            ImGui::EndDisabled();
        }
        ImGui::PopID();
        if (!exchangeMessage_.empty()) {
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::TextWrapped("%s", exchangeMessage_.c_str());
        }
    }

    if (it->status == 0) {
        auto unknown = questGraph_.LockedByPrereqs(it->id, buildStatusMap());
        if (!unknown.empty()) {
            ImGui::Dummy(ImVec2(0, 6));
            std::string reason = "Locked — complete for: ";
            for (size_t i = 0; i < unknown.size(); ++i) {
                if (i > 0) reason += ", ";
                auto pit = std::find_if(quests_.begin(), quests_.end(),
                    [&](const QuestEntry& q) { return q.id == unknown[i]; });
                reason += "#" + std::to_string(unknown[i]) +
                          (pit != quests_.end() ? " " + pit->title : "");
            }
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(160, 160, 160, 255));
            ImGui::TextWrapped("%s", reason.c_str());
            ImGui::PopStyleColor();
        }
    }

    // Manual completion (server-authoritative): shown only for AVAILABLE,
    // non-exchange, non-autoComplete quests. autoComplete quests finish
    // instantly server-side so no button is offered. Local status is updated
    // only on server confirmation (QuestCompletedNotification).
    if (it->status == 1 && !it->isExchange && !it->autoComplete) {
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::PushID(static_cast<int>(it->id));
        if (ImGui::Button("Complete", ImVec2(120, 0))) {
            onCompleteClicked(playerId);
        }
        ImGui::PopID();
    } else if (it->status == 1 && it->autoComplete) {
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(160, 160, 160, 255));
        ImGui::Text("Completes automatically when the objective is met.");
        ImGui::PopStyleColor();
    }

    renderCompletionBadge(it->status);
}

void QuestBookWindow::onCompleteClicked(uint64_t playerId) {
    if (!uiMgr_) {
        spdlog::warn("[Quest] onCompleteClicked: no UIManager");
        return;
    }
    NetClient *net = uiMgr_->GetNetClient();
    if (!net) {
        spdlog::warn("[Quest] onCompleteClicked: no NetClient");
        return;
    }
    net->SendQuestComplete(playerId, selectedQuestId_);
    spdlog::debug("[Quest] Complete requested: player={} quest={}", playerId, selectedQuestId_);
}

void QuestBookWindow::onExchangeClicked(uint64_t playerId) {
    if (!uiMgr_) {
        spdlog::warn("[Quest] onExchangeClicked: no UIManager");
        return;
    }
    NetClient *net = uiMgr_->GetNetClient();
    if (!net) {
        spdlog::warn("[Quest] onExchangeClicked: no NetClient");
        return;
    }
    exchangeMessage_.clear();
    net->SendQuestExchange(playerId, selectedQuestId_);
    spdlog::debug("[Quest] Exchange requested: player={} quest={}", playerId, selectedQuestId_);
}

void QuestBookWindow::onCooldownQuery(uint64_t playerId) {
    if (!uiMgr_) return;
    NetClient *net = uiMgr_->GetNetClient();
    if (!net) return;
    net->SendQuestExchangeCooldownGet(playerId, selectedQuestId_);
}

int QuestBookWindow::countItem(const InventoryState* inv, uint16_t itemId) const {
    if (!inv) return 0;
    int total = 0;
    for (const auto& s : inv->slots) {
        if (s.item_id == itemId) total += s.count;
    }
    return total;
}

std::unordered_map<uint32_t, quest::QuestStatus> QuestBookWindow::buildStatusMap() const {
    std::unordered_map<uint32_t, quest::QuestStatus> statusMap;
    for (const auto& qe : quests_)
        statusMap[qe.id] = static_cast<quest::QuestStatus>(qe.status);
    return statusMap;
}

std::string QuestBookWindow::detectionLabel(quest::DetectionType kind) const {
    switch (kind) {
        case quest::DetectionType::CRAFT: return "Craft";
        case quest::DetectionType::BLOCK_PLACED: return "Place";
        case quest::DetectionType::TOOL_CHARGED: return "Charge tool";
        case quest::DetectionType::SIDE_CONFIGURED: return "Configure side";
        case quest::DetectionType::EXCHANGE: return "Exchange";
        case quest::DetectionType::INVENTORY: return "Obtain";
        case quest::DetectionType::MACHINE: return "Machine output";
        default: return "?";
    }
}

void QuestBookWindow::renderRequirementRow(const quest::QuestRequirement& req,
                                           const InventoryState* playerInv) {
    uint16_t itemId = ItemId::pack(req.item);
    uint16_t need = req.count > 0 ? req.count : 1;
    int have = (req.kind == quest::DetectionType::INVENTORY)
                   ? countItem(playerInv, itemId)
                   : -1;
    bool satisfied = req.kind == quest::DetectionType::INVENTORY &&
                     have >= static_cast<int>(need);
    ImGui::Text("%s", detectionLabel(req.kind).c_str());
    ImGui::SameLine();
    if (itemId != 0) {
        std::string label = itemLabel(itemId, req.item);
        if (req.kind == quest::DetectionType::INVENTORY) {
            ImU32 col = satisfied ? IM_COL32(50, 200, 50, 255)
                                  : IM_COL32(255, 200, 50, 255);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::Text("%s x %u  (have %d)", label.c_str(), need, have);
            ImGui::PopStyleColor();
        } else {
            ImGui::Text("%s x %u", label.c_str(), need);
        }
        ImGui::SameLine();
        auto dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        auto uv = renderlib::TextureAtlas::GetItemUV(itemId);
        dl->AddImage(renderlib::TextureAtlas::GetTextureHandle().idx,
                     ImVec2(p.x, p.y), ImVec2(p.x + 24, p.y + 24),
                     ImVec2(uv.u0, uv.v0), ImVec2(uv.u1, uv.v1));
        char cnt[8];
        std::snprintf(cnt, sizeof(cnt), "%u", need);
        dl->AddText(ImVec2(p.x, p.y + 24), IM_COL32(255, 255, 255, 255), cnt);
        ImGui::Dummy(ImVec2(28, 24));
        if (ImGui::IsItemHovered()) {
            std::string tip = label;
            if (req.kind == quest::DetectionType::MACHINE && !req.machine.empty())
                tip += "\nmachine: " + req.machine;
            if (req.consume) tip += "\nconsumed on completion";
            ImGui::SetTooltip("%s", tip.c_str());
        }
    } else {
        // Unregistered item spec (id 0) → cannot draw an icon; show the raw
        // spec string so the objective stays readable instead of an empty box.
        ImGui::Text("item %s x %u", req.item.c_str(), need);
    }
}

void QuestBookWindow::renderRewardEntry(const quest::RewardEntry& e) {
    if (e.type == quest::RewardType::ITEM) {
        uint16_t itemId = ItemId::pack(e.item);
        uint16_t count = e.count > 0 ? e.count : 1;
        if (itemId != 0) {
            std::string label = itemLabel(itemId, e.item);
            ImGui::Text("%s x %u", label.c_str(), count);
            ImGui::SameLine();
            auto dl = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            auto uv = renderlib::TextureAtlas::GetItemUV(itemId);
            dl->AddImage(renderlib::TextureAtlas::GetTextureHandle().idx,
                         ImVec2(p.x, p.y), ImVec2(p.x + 24, p.y + 24),
                         ImVec2(uv.u0, uv.v0), ImVec2(uv.u1, uv.v1));
            char cnt[8];
            std::snprintf(cnt, sizeof(cnt), "%u", count);
            dl->AddText(ImVec2(p.x, p.y + 24), IM_COL32(255, 255, 255, 255), cnt);
            ImGui::Dummy(ImVec2(28, 24));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", label.c_str());
        } else {
            ImGui::Text("item %s x %u", e.item.c_str(), count);
        }
    } else if (e.type == quest::RewardType::EXPERIENCE) {
        ImGui::Text("EXP: %g", e.value);
    } else {
        ImGui::Text("Special: %s", e.item.c_str());
    }
}

void QuestBookWindow::renderCompletionBadge(uint8_t status) {
    const char* badge = "";
    ImU32 color = IM_COL32(128, 128, 128, 255);
    switch (status) {//TODO use enum
        case 0: badge = "LOCKED";     color = IM_COL32(120, 120, 120, 255); break;
        case 1: badge = "AVAILABLE";  color = IM_COL32(255, 200, 50, 255);  break;
        case 2: badge = "IN PROGRESS"; color = IM_COL32(50, 150, 255, 255); break;
        case 3: badge = "COMPLETED";  color = IM_COL32(50, 200, 50, 255);  break;
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::Text("[%s]", badge);
    ImGui::PopStyleColor();
}

ImU32 QuestBookWindow::statusColor(uint8_t status) const {
    switch (status) {
        case 0: return IM_COL32(120, 120, 120, 255);
        case 1: return IM_COL32(255, 200, 50, 255);
        case 2: return IM_COL32(50, 150, 255, 255);
        case 3: return IM_COL32(50, 200, 50, 255);
        default: return IM_COL32(200, 200, 200, 255);
    }
}

const char* QuestBookWindow::statusLabel(uint8_t status) const {
    switch (status) {
        case 0: return "[LOCKED]";
        case 1: return "[AVAILABLE]";
        case 2: return "[...]";
        case 3: return "[DONE]";
        default: return "[?]";
    }
}

void QuestBookWindow::applyQuestStatus(uint32_t questId, uint8_t status, uint8_t progress) {
    for (auto& qe : quests_) {
        if (qe.id == questId) {
            qe.status = status;
            qe.progress = progress;
            return;
        }
    }
    spdlog::debug("[Quest] Ignoring status for unknown quest {}", questId);
}

int QuestBookWindow::eraIndexFor(uint8_t eraVal) const {
    if (eraVal >= static_cast<uint8_t>(quest::Era::COUNT)) return -1;
    const char* name = quest::EraLabel(static_cast<quest::Era>(eraVal));
    for (size_t i = 0; i < eraData_.size(); ++i) {
        if (eraData_[i].name == name) return static_cast<int>(i);
    }
    return (eraVal < eraData_.size()) ? static_cast<int>(eraVal) : -1;
}

void QuestBookWindow::applyEraTransition(uint8_t completedEra, uint8_t nextEra) {
    if (eraData_.empty()) return;
    int completedIdx = eraIndexFor(completedEra);
    int nextIdx = eraIndexFor(nextEra);
    if (completedIdx >= 0 && static_cast<size_t>(completedIdx) < eraCompleted_.size()) {
        eraCompleted_[completedIdx] = true;
        spdlog::info("[Quest] Era '{}' completed (all quests done)", eraData_[completedIdx].label);
    }
    if (nextIdx >= 0 && nextIdx != completedIdx) {
        newlyAvailableEra_ = nextIdx;
        spdlog::info("[Quest] Era '{}' now available", eraData_[nextIdx].label);
        // Advance the player's current era → unlocks era-gated recipes (UX filter).
        if (uiMgr_) uiMgr_->SetCurrentEra(nextEra);
    }
}

void QuestBookWindow::OnNetworkUpdate(uint8_t msgType, const void* data) {
    if (!data) return;
    switch (msgType) {
        case GatewayMsg::kQuestProgressUpdate: {
            flatbuffers::Verifier v(static_cast<const uint8_t*>(data), 8192);
            if (!v.VerifyBuffer<Protocol::QuestProgressUpdate>(nullptr)) {
                spdlog::warn("QuestBook: invalid QuestProgressUpdate");
                return;
            }
            auto* update = flatbuffers::GetRoot<Protocol::QuestProgressUpdate>(data);
            auto* quests = update->quests();
            if (!quests) return;
            for (size_t i = 0; i < quests->size(); ++i) {
                auto* qe = quests->Get(i);
                if (!qe) continue;
                applyQuestStatus(qe->quest_id(), static_cast<uint8_t>(qe->status()), qe->progress());
            }
            return;
        }
        case GatewayMsg::kQuestUnlockNotification: {
            flatbuffers::Verifier v(static_cast<const uint8_t*>(data), 8192);
            if (!v.VerifyBuffer<Protocol::QuestUnlockNotification>(nullptr)) {
                spdlog::warn("QuestBook: invalid QuestUnlockNotification");
                return;
            }
            auto* unlock = flatbuffers::GetRoot<Protocol::QuestUnlockNotification>(data);
            auto* ids = unlock->unlocked_ids();
            if (!ids) return;
            for (size_t i = 0; i < ids->size(); ++i) {
                applyQuestStatus(ids->Get(i), Protocol::QuestStatus_AVAILABLE, 0);
            }
            return;
        }
        case GatewayMsg::kQuestCompletedNotification: {
            flatbuffers::Verifier v(static_cast<const uint8_t*>(data), 8192);
            if (!v.VerifyBuffer<Protocol::QuestCompletedNotification>(nullptr)) {
                spdlog::warn("QuestBook: invalid QuestCompletedNotification");
                return;
            }
            auto* comp = flatbuffers::GetRoot<Protocol::QuestCompletedNotification>(data);
            applyQuestStatus(comp->quest_id(), Protocol::QuestStatus_COMPLETED, 100);
            return;
        }
        case GatewayMsg::kQuestEraTransition: {
            flatbuffers::Verifier v(static_cast<const uint8_t*>(data), 8192);
            if (!v.VerifyBuffer<Protocol::EraTransitionNotification>(nullptr)) {
                spdlog::warn("QuestBook: invalid EraTransitionNotification");
                return;
            }
            auto* era = flatbuffers::GetRoot<Protocol::EraTransitionNotification>(data);
            applyEraTransition(era->completed_era(), era->next_era());
            return;
        }
        case GatewayMsg::kQuestExchangeResponse: {
            flatbuffers::Verifier v(static_cast<const uint8_t*>(data), 8192);
            if (!v.VerifyBuffer<Protocol::QuestExchangeResponse>(nullptr)) {
                spdlog::warn("QuestBook: invalid QuestExchangeResponse");
                return;
            }
            auto* resp = flatbuffers::GetRoot<Protocol::QuestExchangeResponse>(data);
            if (resp->quest_id() != selectedQuestId_) return;
            cooldownRemainingSecs_ = resp->cooldown_remaining_secs();
            if (resp->success()) {
                exchangeMessage_ = "Exchange complete!";
                spdlog::info("[Quest] Exchange ok: quest={} cooldown={}s",
                             resp->quest_id(), resp->cooldown_remaining_secs());
            } else {
                const char* msg = resp->error_message() ? resp->error_message()->c_str() : "unknown error";
                exchangeMessage_ = std::string("Exchange failed: ") + msg;
                spdlog::warn("[Quest] Exchange failed: quest={} err={}", resp->quest_id(), msg);
            }
            return;
        }
        case GatewayMsg::kQuestExchangeCooldown: {
            flatbuffers::Verifier v(static_cast<const uint8_t*>(data), 8192);
            if (!v.VerifyBuffer<Protocol::QuestExchangeCooldown>(nullptr)) {
                spdlog::warn("QuestBook: invalid QuestExchangeCooldown");
                return;
            }
            auto* cd = flatbuffers::GetRoot<Protocol::QuestExchangeCooldown>(data);
            if (cd->quest_id() != selectedQuestId_) return;
            cooldownRemainingSecs_ = cd->cooldown_remaining_secs();
            spdlog::debug("[Quest] Cooldown for quest={}: {}s", cd->quest_id(), cd->cooldown_remaining_secs());
            return;
        }
        default:
            return;
    }
}

bool QuestBookWindow::OnKeyEvent(int key, int action, int mods) {
    (void)key; (void)action; (void)mods;
    return open_;
}

bool QuestBookWindow::OnMouseClick(int button, int action) {
    (void)button; (void)action;
    return open_;
}
