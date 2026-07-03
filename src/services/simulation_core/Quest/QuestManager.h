#pragma once

#include "quest_lib/QuestData.h"
#include "quest_lib/QuestGraph.h"
#include "quest_lib/QuestTypes.h"
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>
#include <string>
#include <mutex>

namespace simcore {

class QuestManager {
public:
    using PublishCallback = std::function<void(const std::string& topic, const uint8_t* data, size_t len)>;

    QuestManager(quest::QuestData* questData, quest::QuestGraph* questGraph, const PublishCallback& publishCallback)
        : questData_(questData), questGraph_(questGraph), publishCallback_(publishCallback) {}

    ~QuestManager() = default;

    void onPlayerJoined(uint64_t playerId);
    void checkCraftCompletion(uint64_t playerId, uint16_t itemId, uint8_t count);
    void checkBlockAction(uint64_t playerId, int32_t x, int32_t y, int32_t z, uint16_t blockId);
    void loadProgress(uint64_t playerId, const std::vector<uint8_t>& fbData);

private:
    void publishQuestCompleted(uint64_t playerId, uint32_t questId);
    void publishQuestProgressUpdated(uint64_t playerId, uint32_t questId, 
                                     quest::QuestStatus status, uint8_t progress);
    void distributeRewards(uint64_t playerId, const quest::QuestDef& questDef);

private:
    quest::QuestData* questData_;
    quest::QuestGraph* questGraph_;
    PublishCallback publishCallback_;
    std::unordered_map<uint64_t, std::unordered_map<uint32_t, quest::QuestStatus>> progress_;
    std::mutex mutex_;
};

}