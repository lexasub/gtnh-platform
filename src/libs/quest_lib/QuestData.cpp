#include "QuestData.h"
#include "common/ItemId.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

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

        // 9-column schema (detect/reward columns moved to JSON):
        // id,title,description,era,section,cost_item,cost_count,cooldown,target_count
        std::getline(ss, cell, ',');
        qd.id = parseUint(cell, uint32_t{0});

        std::getline(ss, qd.title, ',');

        std::getline(ss, qd.description, ',');

        std::getline(ss, cell, ',');
        qd.era = EraFromString(cell);

        std::getline(ss, qd.section, ',');

        // cost_item / cost_count / cooldown are kept for EXCHANGE quests; none
        // of the current seeds set detectType EXCHANGE, so they stay 0.
        std::getline(ss, cell, ',');
        qd.costItemId = ItemId::pack(cell);

        std::getline(ss, cell, ',');
        qd.costCount = parseUint(cell, uint8_t{0});

        std::getline(ss, cell, ',');
        qd.cooldownSecs = parseUint(cell, uint16_t{0});

        std::getline(ss, cell, ',');
        qd.targetCount = parseUint(cell, uint16_t{0});

        idIndex_[qd.id] = quests_.size();
        quests_.push_back(std::move(qd));
    }

    buildGraph();
    return true;
}

bool QuestData::LoadGraph(const std::string& jsonPath) {
    std::ifstream file(jsonPath);
    if (!file.is_open()) {
        spdlog::error("[QuestData] LoadGraph: cannot open {}", jsonPath);
        return false;
    }

    nlohmann::json root;
    try {
        file >> root;
    } catch (const std::exception& e) {
        spdlog::error("[QuestData] LoadGraph: JSON parse error in {}: {}", jsonPath, e.what());
        return false;
    }

    auto questsIt = root.find("quests");
    if (questsIt == root.end() || !questsIt->is_array()) {
        spdlog::error("[QuestData] LoadGraph: missing or invalid 'quests' array in {}", jsonPath);
        return false;
    }

    auto joinPrereqs = [](const std::vector<uint32_t>& v) {
        std::string s;
        for (size_t i = 0; i < v.size(); ++i) {
            if (i > 0) s += ",";
            s += std::to_string(v[i]);
        }
        return s;
    };

    std::unordered_map<uint32_t, std::vector<uint32_t>> newGraph;
    size_t nodeCount = 0;
    for (const auto& entry : *questsIt) {
        if (!entry.is_object()) continue;
        uint32_t id = entry.value("id", uint32_t{0});
        if (!idIndex_.contains(id)) {
            spdlog::warn("[QuestData] LoadGraph: quest {} in JSON not found in CSV, skipping", id);
            continue;
        }

        std::vector<uint32_t> jsonPrereqs;
        auto prereqsIt = entry.find("prereqs");
        if (prereqsIt != entry.end() && prereqsIt->is_array()) {
            for (const auto& p : *prereqsIt) {
                if (p.is_number_unsigned())
                    jsonPrereqs.push_back(p.get<uint32_t>());
            }
        }

        auto csvPrereqs = GetPrerequisites(id);
        auto sortedJson = jsonPrereqs;
        auto sortedCsv = csvPrereqs;
        std::ranges::sort(sortedJson);
        std::ranges::sort(sortedCsv);
        if (sortedJson != sortedCsv) {
            spdlog::warn("[QuestData] LoadGraph: quest {} prereqs differ: JSON=[{}], CSV=[{}]",
                         id, joinPrereqs(jsonPrereqs), joinPrereqs(csvPrereqs));
        }

        // quest_graph.json is the source of truth for prerequisites — the CSV
        // column is stale in places (e.g. quests 12/13 reference 13 as a
        // self-dependency). Overwrite the CSV value so downstream consumers
        // (QuestGraph::Init via prereqsMap, GetPrerequisites, ...) see the
        // JSON-defined dependencies.
        quests_[idIndex_[id]].prerequisites = jsonPrereqs;

        for (uint32_t prereq : jsonPrereqs) {
            newGraph[prereq].push_back(id);
        }
        ++nodeCount;
    }

    graph_.swap(newGraph);
    spdlog::info("[QuestData] LoadGraph: loaded {} quest nodes from {}", nodeCount, jsonPath);
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

bool QuestData::LoadRequirementsJSON(const std::string& jsonPath) {
    std::ifstream file(jsonPath);
    if (!file.is_open()) {
        spdlog::error("[QuestData] LoadRequirementsJSON: cannot open {}", jsonPath);
        return false;
    }

    nlohmann::json root;
    try {
        file >> root;
    } catch (const std::exception& e) {
        spdlog::error("[QuestData] LoadRequirementsJSON: JSON parse error in {}: {}",
                      jsonPath, e.what());
        return false;
    }

    if (!root.is_object()) {
        spdlog::error("[QuestData] LoadRequirementsJSON: root not an object in {}", jsonPath);
        return false;
    }

    auto parseKind = [](const std::string& k) {
        if (k == "craft") return DetectionType::CRAFT;
        if (k == "obtain") return DetectionType::INVENTORY;
        if (k == "place") return DetectionType::BLOCK_PLACED;
        if (k == "machine") return DetectionType::MACHINE;
        if (k == "side_configured") return DetectionType::SIDE_CONFIGURED;
        if (k == "exchange") return DetectionType::EXCHANGE;
        return DetectionType::CRAFT;
    };

    int merged = 0;
    for (auto it = root.begin(); it != root.end(); ++it) {
        uint32_t id = 0;
        try {
            id = static_cast<uint32_t>(std::stoul(it.key()));
        } catch (...) {
            continue;
        }
        auto qit = idIndex_.find(id);
        if (qit == idIndex_.end()) {
            spdlog::warn("[QuestData] LoadRequirementsJSON: quest {} not loaded, skipping", id);
            continue;
        }

        auto& qd = quests_[qit->second];
        if (it.value().contains("auto_complete") && it.value()["auto_complete"].is_boolean()) {
            qd.autoComplete = it.value()["auto_complete"].get<bool>();
        }

        auto reqsIt = it.value().find("requirements");
        if (reqsIt == it.value().end() || !reqsIt->is_array()) continue;

        qd.requirements.clear();
        for (const auto& r : *reqsIt) {
            QuestRequirement req;
            req.kind = parseKind(r.value("kind", std::string{}));
            req.item = r.value("item", std::string{});
            req.count = static_cast<uint16_t>(r.value("count", uint32_t{0}));
            req.consume = r.value("consume", false);
            req.machine = r.value("machine", std::string{});
            qd.requirements.push_back(std::move(req));
        }

        // Merge the primary requirement into the legacy detection fields so
        // existing trigger handlers keep firing unchanged.
        auto& primary = qd.requirements.front();
        qd.detectType = primary.kind;
        qd.detectTarget = primary.item;
        qd.targetCount = primary.count;
        ++merged;
    }

    spdlog::info("[QuestData] LoadRequirementsJSON: merged requirements for {} quests from {}",
                 merged, jsonPath);
    return true;
}

bool QuestData::LoadRewardsJSON(const std::string& jsonPath) {
    std::ifstream file(jsonPath);
    if (!file.is_open()) {
        spdlog::error("[QuestData] LoadRewardsJSON: cannot open {}", jsonPath);
        return false;
    }

    nlohmann::json root;
    try {
        file >> root;
    } catch (const std::exception& e) {
        spdlog::error("[QuestData] LoadRewardsJSON: JSON parse error in {}: {}", jsonPath, e.what());
        return false;
    }

    if (!root.is_object()) {
        spdlog::error("[QuestData] LoadRewardsJSON: root not an object in {}", jsonPath);
        return false;
    }

    auto parseType = [](const std::string& t) {
        if (t == "experience") return RewardType::EXPERIENCE;
        if (t == "special") return RewardType::SPECIAL;
        return RewardType::ITEM;
    };

    auto parseEntry = [&parseType](const nlohmann::json& e) {
        RewardEntry entry;
        entry.type = parseType(e.value("type", std::string{"item"}));
        entry.item = e.value("item", std::string{});
        entry.count = static_cast<uint16_t>(e.value("count", uint32_t{0}));
        entry.value = static_cast<float>(e.value("value", 0.0));
        return entry;
    };

    rewards_.clear();
    int loaded = 0;
    for (auto it = root.begin(); it != root.end(); ++it) {
        uint32_t id = 0;
        try {
            id = static_cast<uint32_t>(std::stoul(it.key()));
        } catch (...) {
            continue;
        }

        QuestReward qr;
        if (it.value().contains("rewards") && it.value()["rewards"].is_array()) {
            for (const auto& e : it.value()["rewards"]) qr.rewards.push_back(parseEntry(e));
        } else if (it.value().contains("choice_of") && it.value()["choice_of"].is_array()) {
            for (const auto& e : it.value()["choice_of"]) qr.choiceOf.push_back(parseEntry(e));
        }
        rewards_[id] = std::move(qr);
        ++loaded;
    }

    spdlog::info("[QuestData] LoadRewardsJSON: loaded rewards for {} quests from {}", loaded, jsonPath);
    return true;
}

const QuestReward* QuestData::GetReward(uint32_t questId) const {
    auto it = rewards_.find(questId);
    if (it == rewards_.end()) return nullptr;
    return &it->second;
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

    for (int e = 0; e < static_cast<int>(Era::COUNT); ++e) {
        Era era = static_cast<Era>(e);

        EraInfo ei;
        ei.name = EraLabel(era);
        ei.label = ei.name;

        for (const auto& qd : quests_) {
            if (qd.era != era) continue;

            auto section = std::ranges::find_if(ei.sections, [&](const SectionInfo& si) {
                return si.name == qd.section;
            });
            if (section == ei.sections.end()) {
                SectionInfo si;
                si.name = qd.section;
                si.label = qd.section;
                if (!si.label.empty()) {
                    si.label[0] = static_cast<char>(std::toupper(si.label[0]));
                }
                ei.sections.push_back(std::move(si));
                section = std::prev(ei.sections.end());
            }
            section->questIds.push_back(qd.id);
        }

        if (!ei.sections.empty()) eras.push_back(std::move(ei));
    }
    return eras;
}

std::vector<std::string> QuestData::SectionsForEra(Era era) const {
    std::vector<std::string> result;
    for (const auto& qd : quests_) {
        if (qd.era == era) {
            if (std::ranges::find(result, qd.section) == result.end())
                result.push_back(qd.section);
        }
    }
    return result;
}

std::unordered_map<uint32_t, Era> QuestData::BuildQuestEraMap() const {
    std::unordered_map<uint32_t, Era> map;
    map.reserve(quests_.size());
    for (const auto& qd : quests_) {
        if (qd.detectType == DetectionType::EXCHANGE)
            continue;
        map[qd.id] = qd.era;
    }
    return map;
}

}
