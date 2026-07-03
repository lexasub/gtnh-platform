#pragma once

#include "QuestTypes.h"
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace quest {

// Evaluates DAG conditions: which quests unlock given current progress.
class QuestGraph {
public:
    QuestGraph() = default;

    void Init(const std::unordered_map<uint32_t, std::vector<uint32_t>>& graph,
              const std::unordered_map<uint32_t, std::vector<uint32_t>>& prerequisites);

    std::vector<uint32_t> NewlyAvailable(
        const std::unordered_map<uint32_t, QuestStatus>& current) const;

    bool CanComplete(uint32_t questId,
                     const std::unordered_map<uint32_t, QuestStatus>& current) const;

    bool IsEraComplete(Era era,
                       const std::unordered_map<uint32_t, QuestStatus>& current,
                       const std::unordered_map<uint32_t, Era>& questEraMap) const;

    std::vector<uint32_t> GetUnlocked(
        const std::unordered_map<uint32_t, QuestStatus>& current) const;

private:
    std::unordered_map<uint32_t, std::vector<uint32_t>> children_;
    std::unordered_map<uint32_t, std::vector<uint32_t>> prereqs_;
};

}
