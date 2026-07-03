#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace quest {

enum class QuestStatus : uint8_t {
    LOCKED = 0,
    AVAILABLE = 1,
    IN_PROGRESS = 2,
    COMPLETED = 3,
};

enum class Era : uint8_t {
    VAGRANT = 0,
    APPRENTICE = 1,
    EXPERT = 2,
    ADMINISTRATOR = 3,
};

inline const char* EraLabel(Era e) {
    switch (e) {
        case Era::VAGRANT:     return "Vagrant";
        case Era::APPRENTICE:  return "Apprentice";
        case Era::EXPERT:      return "Expert";
        case Era::ADMINISTRATOR: return "Administrator";
    }
    return "Unknown";
}

inline Era EraFromString(const std::string& s) {
    if (s == "vagrant")     return Era::VAGRANT;
    if (s == "apprentice")  return Era::APPRENTICE;
    if (s == "expert")      return Era::EXPERT;
    if (s == "administrator") return Era::ADMINISTRATOR;
    return Era::VAGRANT;
}

enum class DetectionType : uint8_t {
    CRAFT = 0,
    BLOCK_PLACED = 1,
    TOOL_CHARGED = 2,
    SIDE_CONFIGURED = 3,
};

inline DetectionType DetectFromString(const std::string& s) {
    if (s == "craft")          return DetectionType::CRAFT;
    if (s == "block_placed")   return DetectionType::BLOCK_PLACED;
    if (s == "tool_charged")   return DetectionType::TOOL_CHARGED;
    if (s == "side_configured") return DetectionType::SIDE_CONFIGURED;
    return DetectionType::CRAFT;
}

struct QuestDef {
    uint32_t id = 0;
    std::string title;
    std::string description;
    Era era = Era::VAGRANT;
    std::string section;
    std::vector<uint32_t> prerequisites;
    DetectionType detectType = DetectionType::CRAFT;
    std::string detectTarget;
    uint16_t rewardItemId = 0;
    uint8_t rewardCount = 0;
};

struct QuestProgress {
    uint32_t questId = 0;
    QuestStatus status = QuestStatus::LOCKED;
    uint8_t progressPercent = 0;
};

struct QuestProgressSnapshot {
    uint64_t playerId = 0;
    std::vector<QuestProgress> entries;
};

struct SectionInfo {
    std::string name;
    std::string label;
    std::vector<uint32_t> questIds;
};

struct EraInfo {
    std::string name;
    std::string label;
    std::vector<SectionInfo> sections;
};

}
