#include "QuestData.h"
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>

namespace quest {

bool QuestData::LoadCSV(const std::string& csvPath) {
    std::ifstream file(csvPath);
    if (!file.is_open()) return false;

    quests_.clear();
    idIndex_.clear();

    std::string line;
    std::getline(file, line);

    while (std::getline(file, line)) {
        if (line.empty()) continue;

        std::stringstream ss(line);
        std::string cell;
        QuestDef qd;

        auto parseUint = [](const std::string& s, auto defaultVal) {
            if (s.empty()) return static_cast<decltype(defaultVal)>(0);
            try {
                return static_cast<decltype(defaultVal)>(std::stoul(s));
            } catch (...) {
                return static_cast<decltype(defaultVal)>(0);
            }
        };

        std::getline(ss, cell, ',');
        qd.id = parseUint(cell, uint32_t{0});

        std::getline(ss, qd.title, ',');

        std::getline(ss, qd.description, ',');

        std::getline(ss, cell, ',');
        qd.era = EraFromString(cell);

        std::getline(ss, qd.section, ',');

        std::getline(ss, cell, ',');
        if (!cell.empty()) {
            std::stringstream ps(cell);
            std::string pid;
            while (std::getline(ps, pid, ';')) {
                if (!pid.empty())
                    qd.prerequisites.push_back(parseUint(pid, uint32_t{0}));
            }
        }

        std::getline(ss, cell, ',');
        qd.detectType = DetectFromString(cell);

        std::getline(ss, qd.detectTarget, ',');

        std::getline(ss, cell, ',');
        qd.rewardItemId = parseUint(cell, uint16_t{0});

        std::getline(ss, cell, ',');
        qd.rewardCount = parseUint(cell, uint8_t{0});

        idIndex_[qd.id] = quests_.size();
        quests_.push_back(std::move(qd));
    }

    buildGraph();
    return true;
}

bool QuestData::LoadGraph(const std::string& /*jsonPath*/) {
    return true;
}

void QuestData::buildGraph() {
    graph_.clear();
    for (const auto& qd : quests_) {
        for (uint32_t prereq : qd.prerequisites) {
            graph_[prereq].push_back(qd.id);
        }
    }
}

const QuestDef* QuestData::GetQuest(uint32_t id) const {
    auto it = idIndex_.find(id);
    if (it == idIndex_.end()) return nullptr;
    return &quests_[it->second];
}

std::vector<const QuestDef*> QuestData::GetEraQuests(Era era) const {
    std::vector<const QuestDef*> result;
    for (const auto& qd : quests_) {
        if (qd.era == era)
            result.push_back(&qd);
    }
    return result;
}

std::vector<const QuestDef*> QuestData::GetSectionQuests(const std::string& section) const {
    std::vector<const QuestDef*> result;
    for (const auto& qd : quests_) {
        if (qd.section == section)
            result.push_back(&qd);
    }
    return result;
}

const std::vector<uint32_t>& QuestData::GetPrerequisites(uint32_t questId) const {
    static const std::vector<uint32_t> empty;
    auto* qd = GetQuest(questId);
    if (!qd) return empty;
    return qd->prerequisites;
}

std::vector<uint32_t> QuestData::GetChildren(uint32_t questId) const {
    auto it = graph_.find(questId);
    if (it == graph_.end()) return {};
    return it->second;
}

std::vector<uint32_t> QuestData::GetRootQuests() const {
    std::vector<uint32_t> roots;
    for (const auto& qd : quests_) {
        if (qd.prerequisites.empty())
            roots.push_back(qd.id);
    }
    return roots;
}

std::vector<EraInfo> QuestData::BuildEraStructure() const {
    std::vector<EraInfo> eras;
    struct EraAccum {
        std::unordered_map<std::string, std::vector<uint32_t>> sections;
    };
    std::unordered_map<int, EraAccum> accum;

    for (const auto& qd : quests_) {
        accum[static_cast<int>(qd.era)].sections[qd.section].push_back(qd.id);
    }

    for (int e = 0; e <= static_cast<int>(Era::ADMINISTRATOR); ++e) {
        auto ea = accum.find(e);
        if (ea == accum.end()) continue;
        Era era = static_cast<Era>(e);

        EraInfo ei;
        ei.name = EraLabel(era);
        ei.label = ei.name;

        for (auto& [secName, ids] : ea->second.sections) {
            SectionInfo si;
            si.name = secName;
            si.label = secName;
            if (!si.label.empty()) {
                si.label[0] = static_cast<char>(std::toupper(si.label[0]));
            }
            si.questIds = std::move(ids);
            ei.sections.push_back(std::move(si));
        }
        eras.push_back(std::move(ei));
    }
    return eras;
}

std::vector<std::string> QuestData::SectionsForEra(Era era) const {
    std::vector<std::string> result;
    for (const auto& qd : quests_) {
        if (qd.era == era) {
            if (std::find(result.begin(), result.end(), qd.section) == result.end())
                result.push_back(qd.section);
        }
    }
    return result;
}

}
