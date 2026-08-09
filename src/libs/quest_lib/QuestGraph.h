#pragma once

#include "QuestTypes.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace quest {

// Evaluates DAG conditions: which quests unlock given current progress.
class QuestGraph {
public:
  QuestGraph() = default;

  void Init(
      const std::unordered_map<uint32_t, std::vector<uint32_t>> &graph,
      const std::unordered_map<uint32_t, std::vector<uint32_t>> &prerequisites);

  std::vector<uint32_t> NewlyAvailable(
      const std::unordered_map<uint32_t, QuestStatus> &current) const;

  bool
  CanComplete(uint32_t questId,
              const std::unordered_map<uint32_t, QuestStatus> &current) const;

  bool
  IsEraComplete(Era era,
                const std::unordered_map<uint32_t, QuestStatus> &current,
                const std::unordered_map<uint32_t, Era> &questEraMap) const;

  // Returns prerequisite quest ids that are not yet COMPLETED — the reasons
  // `questId` is LOCKED. Empty when every prerequisite is completed. Prereqs
  // absent from `current` are treated as not completed.
  std::vector<uint32_t>
  LockedByPrereqs(uint32_t questId,
                  const std::unordered_map<uint32_t, QuestStatus> &current) const;

  std::vector<uint32_t>
  GetUnlocked(const std::unordered_map<uint32_t, QuestStatus> &current) const;

private:
  std::unordered_map<uint32_t, std::vector<uint32_t>> children_;
  std::unordered_map<uint32_t, std::vector<uint32_t>> prereqs_;
};

} // namespace quest
