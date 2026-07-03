#pragma once

#include "QuestTypes.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace quest {

class QuestData {
public:
    bool LoadCSV(const std::string& csvPath);
    bool LoadGraph(const std::string& jsonPath);

    const QuestDef* GetQuest(uint32_t id) const;
    std::vector<const QuestDef*> GetEraQuests(Era era) const;
    std::vector<const QuestDef*> GetSectionQuests(const std::string& section) const;
    const std::vector<uint32_t>& GetPrerequisites(uint32_t questId) const;
    std::vector<uint32_t> GetChildren(uint32_t questId) const;
    std::vector<uint32_t> GetRootQuests() const;

    const std::vector<QuestDef>& AllQuests() const { return quests_; }
    const std::unordered_map<uint32_t, std::vector<uint32_t>>& Graph() const { return graph_; }

    std::vector<EraInfo> BuildEraStructure() const;
    std::vector<std::string> SectionsForEra(Era era) const;

    size_t Count() const { return quests_.size(); }

private:
    std::vector<QuestDef> quests_;
    std::unordered_map<uint32_t, size_t> idIndex_;
    std::unordered_map<uint32_t, std::vector<uint32_t>> graph_;

    void buildGraph();
};

}
