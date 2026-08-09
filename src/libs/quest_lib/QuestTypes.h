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
  ENERGY_JUNIOR = 4,
  ENERGY_MIDDLE = 5,
  ENERGY_SENIOR = 6,
  COUNT,
};

inline const char *EraLabel(Era e) {
  switch (e) {
  case Era::VAGRANT:
    return "Vagrant";
  case Era::APPRENTICE:
    return "Apprentice";
  case Era::EXPERT:
    return "Expert";
  case Era::ADMINISTRATOR:
    return "Administrator";
  case Era::ENERGY_JUNIOR:
    return "Energy Junior";
  case Era::ENERGY_MIDDLE:
    return "Energy Middle";
  case Era::ENERGY_SENIOR:
    return "Energy Senior";
  case Era::COUNT:
    break;
  }
  return "Unknown";
}

inline Era EraFromString(const std::string &s) {
  if (s == "vagrant")
    return Era::VAGRANT;
  if (s == "apprentice")
    return Era::APPRENTICE;
  if (s == "expert")
    return Era::EXPERT;
  if (s == "administrator")
    return Era::ADMINISTRATOR;
  if (s == "energy_junior")
    return Era::ENERGY_JUNIOR;
  if (s == "energy_middle")
    return Era::ENERGY_MIDDLE;
  if (s == "energy_senior")
    return Era::ENERGY_SENIOR;
  return Era::VAGRANT;
}

enum class DetectionType : uint8_t {
  CRAFT = 0,
  BLOCK_PLACED = 1,
  TOOL_CHARGED = 2,
  SIDE_CONFIGURED = 3,
  EXCHANGE = 4,
  INVENTORY = 5,
  MACHINE = 6,
};

inline DetectionType DetectFromString(const std::string &s) {
  if (s == "craft")
    return DetectionType::CRAFT;
  if (s == "block_placed")
    return DetectionType::BLOCK_PLACED;
  if (s == "tool_charged")
    return DetectionType::TOOL_CHARGED;
  if (s == "side_configured")
    return DetectionType::SIDE_CONFIGURED;
  if (s == "exchange")
    return DetectionType::EXCHANGE;
  if (s == "inventory")
    return DetectionType::INVENTORY;
  if (s == "machine")
    return DetectionType::MACHINE;
  return DetectionType::CRAFT;
}

// A single quest objective requirement. Populated from quest_requirements.json;
// also feeds QuestDef.detectType/detectTarget/targetCount so existing
// trigger handlers (checkCraftCompletion et al.) keep firing unchanged.
struct QuestRequirement {
  DetectionType kind = DetectionType::CRAFT;
  std::string item;    // hierarchical spec string, e.g. "0:10:11:2"
  uint16_t count = 0;  // INVENTORY/MACHINE objective quantity; else informational
  bool consume = false; // item taken on completion (default false)
  std::string machine; // machine spec string; only for kind MACHINE
};

enum class RewardType : uint8_t {
  ITEM = 0,
  EXPERIENCE = 1,
  SPECIAL = 2,
};

struct RewardEntry {
  RewardType type = RewardType::ITEM;
  std::string item;     // for ITEM
  uint16_t count = 0;   // for ITEM
  float value = 0.f;    // for EXPERIENCE (amount of XP)
};

// One quest's reward set: either all of `rewards` are granted, or the player
// picks one of `choiceOf` (XOR enforced by the loader). Loaded from
// quest_rewards.json.
struct QuestReward {
  std::vector<RewardEntry> rewards;
  std::vector<RewardEntry> choiceOf;
};

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
  uint16_t costItemId = 0;
  uint8_t costCount = 0;
  uint16_t cooldownSecs = 0;
  // DetectionType::INVENTORY objective: quantity of detectTarget the player
  // must hold in their inventory to complete (default 0 → treated as ≥1).
  uint16_t targetCount = 0;
  // True: requirement met + prerequisites OK → complete immediately (default).
  // False: requirement met → transition to AVAILABLE; player must press
  // Complete (completeQuest) to finish. Populated from quest_requirements.json.
  bool autoComplete = true;
  // Structured objectives from quest_requirements.json. As a single source of
  // truth it also populates detectType/detectTarget/targetCount above.
  std::vector<QuestRequirement> requirements;
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

} // namespace quest
