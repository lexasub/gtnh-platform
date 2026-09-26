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

    // gp-kjfh: walk the ERA's quest set, not the player's progress map.
    //
    // The old loop iterated `current` and only failed on quests it happened to
    // FIND there, so any quest of the era missing from the player's state was
    // silently skipped and a PARTIALLY-SEEDED state read as vacuously
    // complete. That is reachable in production: onPlayerJoined and the craft
    // request path are independent messages with no ordering guarantee
    // (QuestManager.h:71 documents the pre-join craft as supported), and
    // completeQuestInternal seeds only the ONE quest it completes. A pre-join
    // craft therefore published quest.era.transition on the era's FIRST quest,
    // and the bogus answer was LATCHED into completedEras_, so the genuine
    // completion never published.
    //
    // questEraMap is the right thing to iterate: BuildQuestEraMap
    // (QuestData.cpp:393-402) derives the era membership from every loaded
    // quest, not from any one player, so it is the authority on "which quests
    // belong to this era" independent of how much of the graph a given player
    // has seeded.
    //
    // A quest of the era that is ABSENT from `current` counts as not
    // completed, matching the rule LockedByPrereqs already documents for
    // absent prerequisites (QuestGraph.h:31-33) and CanComplete applies
    // (QuestGraph.cpp:20). An era with no quests of its own is still complete:
    // the loop has nothing of this era to fail on, exactly as a quest with no
    // prerequisites can be completed.
    for (const auto& [questId, eraOfQuest] : questEraMap) {
        if (eraOfQuest != era) continue;
        auto it = current.find(questId);
        if (it == current.end() || it->second != QuestStatus::COMPLETED)
            return false;
    }
    return true;
}

std::vector<uint32_t> QuestGraph::LockedByPrereqs(
    uint32_t questId,
    const std::unordered_map<uint32_t, QuestStatus>& current) const {

    std::vector<uint32_t> blocked;
    auto it = prereqs_.find(questId);
    if (it == prereqs_.end()) return blocked;
    for (uint32_t prereq : it->second) {
        auto statusIt = current.find(prereq);
        if (statusIt == current.end() || statusIt->second != QuestStatus::COMPLETED)
            blocked.push_back(prereq);
    }
    return blocked;
}

}
