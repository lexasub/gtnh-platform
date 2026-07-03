#include "QuestBookWindow.h"
#include "quest_lib/QuestData.h"
#include "quest_lib/QuestGraph.h"
#include <imgui.h>
#include <spdlog/spdlog.h>
#include <string>

QuestBookWindow::QuestBookWindow() {
    loadQuestData();
}

void QuestBookWindow::SetOpen(bool open) {
    open_ = open;
}

void QuestBookWindow::loadQuestData() {
    quest::QuestData qd;
    std::string dataDir = DATA_DIR;
    if (!qd.LoadCSV(dataDir + "/quests/quests.csv")) {
        spdlog::error("[Quest] Failed to load quests.csv");
        return;
    }
    qd.LoadGraph(dataDir + "/quests/quest_graph.json");

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
        e.status = 0;
        e.progress = 0;
        quests_.push_back(e);
    }

    eraData_ = qd.BuildEraStructure();
    dataLoaded_ = true;
    spdlog::info("[Quest] Loaded {} quests across {} eras", quests_.size(), eraData_.size());
}

void QuestBookWindow::Render(InventoryState*) {
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
        renderQuestDetail();
    }
    ImGui::EndChild();

    ImGui::End();
}

void QuestBookWindow::renderEraTabs() {
    if (eraData_.empty()) return;
    if (ImGui::BeginTabBar("eraTabs")) {
        for (size_t i = 0; i < eraData_.size(); ++i) {
            if (ImGui::TabItemButton(eraData_[i].label.c_str(),
                i == static_cast<size_t>(selectedEra_) ? ImGuiTabItemFlags_SetSelected : 0)) {
                selectedEra_ = static_cast<int>(i);
                selectedSection_ = 0;
                selectedQuestId_ = 0;
            }
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
        }
        ImGui::PopStyleColor();
    }
}

void QuestBookWindow::renderQuestDetail() {
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

    if (it->rewardItemId > 0) {
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Text("Reward: item %u x %u", it->rewardItemId, it->rewardCount);
    }

    renderCompletionBadge(it->status);
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

void QuestBookWindow::updateQuestStatus(const std::vector<uint8_t>& fbData) {
    if (fbData.size() < 8) return;
    const uint8_t* ptr = fbData.data();
    uint32_t questId = ptr[0] | (static_cast<uint32_t>(ptr[1]) << 8)
                     | (static_cast<uint32_t>(ptr[2]) << 16) | (static_cast<uint32_t>(ptr[3]) << 24);
    uint8_t status = ptr[4];
    uint8_t progress = ptr[5];

    for (auto& qe : quests_) {
        if (qe.id == questId) {
            qe.status = status;
            qe.progress = progress;
            break;
        }
    }
}

void QuestBookWindow::OnNetworkUpdate(uint8_t msgType, const void* data) {
    if (!data) return;
    if (msgType == 19) {//TODO use enum val
        auto* bytes = static_cast<const uint8_t*>(data);
        std::vector<uint8_t> fb(bytes, bytes + 1024);
        updateQuestStatus(fb);
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
