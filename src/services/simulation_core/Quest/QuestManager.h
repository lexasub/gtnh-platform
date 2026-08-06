#pragma once

#include "quest_lib/QuestData.h"
#include "quest_lib/QuestGraph.h"
#include "quest_lib/QuestTypes.h"
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace simcore {

// Player inventory slot (see Storage/PlayerInventoryStore.h). Forward-declared
// so the header stays dependency-light; QuestManager.cpp includes the full
// definition.
struct PersistSlot;

class QuestManager {
public:
  using PublishCallback = std::function<void(const std::string &topic,
                                             const uint8_t *data, size_t len)>;

  QuestManager(quest::QuestData *questData, quest::QuestGraph *questGraph,
               const PublishCallback &publishCallback)
      : questData_(questData), questGraph_(questGraph),
        publishCallback_(publishCallback) {}

  ~QuestManager() = default;

  void onPlayerJoined(uint64_t playerId);
  void checkCraftCompletion(uint64_t playerId, uint16_t itemId, uint8_t count);
  void checkBlockAction(uint64_t playerId, int32_t x, int32_t y, int32_t z,
                        uint16_t blockId);
  // Detection handler for DetectionType::TOOL_CHARGED. `itemId` is the packed
  // tool id; the quest completes when the tool's detectTarget matches and
  // prerequisites are met.
  void checkToolCharged(uint64_t playerId, uint16_t itemId);
  // Detection handler for DetectionType::SIDE_CONFIGURED. `machineId` is the
  // packed block id of the machine whose face was cycled (0 for hatches).
  void checkSideConfigured(uint64_t playerId, uint16_t machineId);
  // Detection handler for DetectionType::INVENTORY. `slots` is a snapshot of
  // the player's current inventory; each INVENTORY-type quest whose
  // detectTarget item is held in quantity >= targetCount (targetCount 0 →
  // ≥1) completes when prerequisites are met. Triggered when the player opens
  // the quest book.
  void checkInventory(uint64_t playerId, const std::vector<PersistSlot> &slots);
  void loadProgress(uint64_t playerId, const std::vector<uint8_t> &fbData);

  // Server-authoritative manual completion (client "Complete" button).
  // Validates the quest exists, is AVAILABLE, and its prerequisites are met
  // (QuestGraph::CanComplete). On acceptance transitions AVAILABLE→COMPLETED,
  // publishes quest.completed + quest.progress.updated, and unlocks any newly
  // available dependents (quest.unlocked). Returns false (no state change, no
  // reward) on rejection. Idempotent: already-COMPLETED quests are rejected.
  bool completeQuest(uint64_t playerId, uint32_t questId);

private:
  // Shared one-step completion. Transitions the quest (any non-COMPLETED
  // status) to COMPLETED when prerequisites are met, publishes quest.completed
  // + quest.progress.updated, evaluates era transition, distributes rewards,
  // and cascades unlocks via QuestGraph::NewlyAvailable() on quest.unlocked.
  // Seeds missing quests as LOCKED so detection works even before onPlayerJoined.
  // Returns false (no state change) for already-COMPLETED quests.
  bool completeQuestInternal(uint64_t playerId, uint32_t questId);
  void publishQuestCompleted(uint64_t playerId, uint32_t questId);
  void publishQuestProgressUpdated(uint64_t playerId, uint32_t questId,
                                   quest::QuestStatus status, uint8_t progress);
  void publishQuestUnlocked(uint64_t playerId,
                            const std::vector<uint32_t> &questIds);
  void publishEraTransition(uint64_t playerId, quest::Era completedEra);
  void maybePublishEraTransition(uint64_t playerId, uint32_t questId);
  void rebuildCompletedEras(uint64_t playerId,
                            const std::unordered_map<uint32_t, quest::QuestStatus> &playerProgress);
  void distributeRewards(uint64_t playerId, const quest::QuestDef &questDef);

private:
  quest::QuestData *questData_;
  quest::QuestGraph *questGraph_;
  PublishCallback publishCallback_;
  std::unordered_map<uint64_t, std::unordered_map<uint32_t, quest::QuestStatus>>
      progress_;
  std::unordered_map<uint64_t, std::unordered_set<uint8_t>> completedEras_;
  std::unordered_map<uint32_t, quest::Era> questEraMap_;
  std::mutex mutex_;
};

} // namespace simcore