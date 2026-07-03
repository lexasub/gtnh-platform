#include "QuestGraph.h"

namespace quest {

void QuestGraph::Init(
    const std::unordered_map<uint32_t, std::vector<uint32_t>>& graph,
    const std::unordered_map<uint32_t, std::vector<uint32_t>>& prerequisites) {
    children_ = graph;
    prereqs_ = prerequisites;
}

bool QuestGraph::CanComplete(
    uint32_t questId,
    const std::unordered_map<uint32_t, QuestStatus>& current) const {

    auto it = prereqs_.find(questId);
    if (it == prereqs_.end()) return true;
    for (uint32_t prereq : it->second) {
        auto statusIt = current.find(prereq);
        if (statusIt == current.end() || statusIt->second != QuestStatus::COMPLETED)
            return false;
    }
    return true;
}

std::vector<uint32_t> QuestGraph::NewlyAvailable(
    const std::unordered_map<uint32_t, QuestStatus>& current) const {

    std::vector<uint32_t> result;
    for (const auto& [questId, status] : current) {
        if (status != QuestStatus::LOCKED) continue;
        if (CanComplete(questId, current))
            result.push_back(questId);
    }
    return result;
}

std::vector<uint32_t> QuestGraph::GetUnlocked(
    const std::unordered_map<uint32_t, QuestStatus>& current) const {

    std::vector<uint32_t> result;
    for (const auto& [questId, status] : current) {
        if (status != QuestStatus::LOCKED && status != QuestStatus::AVAILABLE) continue;
        if (CanComplete(questId, current)) {
            if (status == QuestStatus::LOCKED)
                result.push_back(questId);
        }
    }
    return result;
}

bool QuestGraph::IsEraComplete(
    Era era,
    const std::unordered_map<uint32_t, QuestStatus>& current,
    const std::unordered_map<uint32_t, Era>& questEraMap) const {

    for (const auto& [questId, status] : current) {
        auto eraIt = questEraMap.find(questId);
        if (eraIt == questEraMap.end()) continue;
        if (eraIt->second != era) continue;
        if (status != QuestStatus::COMPLETED) return false;
    }
    return true;
}

}
