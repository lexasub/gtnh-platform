#pragma once

#include "QuestTypes.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace quest {

class QuestData {
public:
  bool LoadCSV(const std::string& csvPath);
  bool LoadGraph(const std::string& jsonPath);
  // Loads data/quests/quest_requirements.json and merges each quest's
  // requirements + auto_complete into its QuestDef: kind → detectType, item →
  // detectTarget, count → targetCount. Safe to call after LoadCSV+LoadGraph.
  bool LoadRequirementsJSON(const std::string& jsonPath);
  // Loads data/quests/quest_rewards.json into the questId → reward map.
  bool LoadRewardsJSON(const std::string& jsonPath);

  const QuestDef *GetQuest(uint32_t id) const;
  std::vector<const QuestDef *> GetEraQuests(Era era) const;
  std::vector<const QuestDef *>
  GetSectionQuests(const std::string& section) const;
  const std::vector<uint32_t>& GetPrerequisites(uint32_t questId) const;
  std::vector<uint32_t> GetChildren(uint32_t questId) const;
  std::vector<uint32_t> GetRootQuests() const;

  const std::vector<QuestDef>& AllQuests() const { return quests_; }
  const std::unordered_map<uint32_t, std::vector<uint32_t>>& Graph() const {
    return graph_;
  }
  // Empty QuestReward (no rewards / choice) unless the quest appears in
  // quest_rewards.json.
  const QuestReward* GetReward(uint32_t questId) const;

  std::vector<EraInfo> BuildEraStructure() const;
  std::vector<std::string> SectionsForEra(Era era) const;

  // Builds quest_id → Era lookup map. Used by QuestGraph::IsEraComplete() to
  // determine which era a quest belongs to when checking era completion.
  std::unordered_map<uint32_t, Era> BuildQuestEraMap() const;

  size_t Count() const { return quests_.size(); }

private:
  std::vector<QuestDef> quests_;
  std::unordered_map<uint32_t, size_t> idIndex_;
  std::unordered_map<uint32_t, std::vector<uint32_t>> graph_;
  std::unordered_map<uint32_t, QuestReward> rewards_;

  void buildGraph();
};

} // namespace quest
