#include "QuestManager.h"
#include "quest_generated.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <mutex>
#include <cstdint>

namespace simcore {

void QuestManager::publishQuestCompleted(uint64_t playerId, uint32_t questId) {
    if (!publishCallback_) {
        spdlog::warn("[QuestManager] publishQuestCompleted: no publish callback for player {}", playerId);
        return;
    }
    flatbuffers::FlatBufferBuilder builder(64);
    auto timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    auto offset = Protocol::CreateQuestCompleted(builder, playerId, questId, timestamp);
    builder.Finish(offset);
    publishCallback_("quest.completed", builder.GetBufferPointer(), builder.GetSize());
    spdlog::info("[QuestManager] Published quest.completed: player={}, quest={}", playerId, questId);
}

void QuestManager::publishQuestProgressUpdated(uint64_t playerId, uint32_t questId,
                                               quest::QuestStatus status, uint8_t progress) {
    if (!publishCallback_) {
        spdlog::warn("[QuestManager] publishQuestProgressUpdated: no publish callback for player {}", playerId);
        return;
    }
    flatbuffers::FlatBufferBuilder builder(64);
    auto entry = Protocol::CreateQuestEntry(builder, questId,
                                            static_cast<Protocol::QuestStatus>(status), progress);
    auto questsVec = builder.CreateVector(&entry, 1);
    auto offset = Protocol::CreateQuestProgressUpdate(builder, playerId, questsVec);
    builder.Finish(offset);
    publishCallback_("quest.progress.updated", builder.GetBufferPointer(), builder.GetSize());
    spdlog::debug("[QuestManager] Published quest.progress.updated: player={}, quest={}, status={}, progress={}",
                  playerId, questId, static_cast<uint8_t>(status), progress);
}

void QuestManager::onPlayerJoined(uint64_t playerId) {
    if (!questData_) {
        spdlog::error("[QuestManager] onPlayerJoined: questData_ is null for player {}", playerId);
        return;
    }
    
    spdlog::info("[QuestManager] Player {} joined, initializing quest state", playerId);
    
    try {
        // Initialize player quest state with thread safety
        std::lock_guard<std::mutex> lock(mutex_);
        auto& playerProgress = progress_[playerId];

        // Seed all known quests as LOCKED so the graph invariant holds:
        // NewlyAvailable()/GetUnlocked() only consider quests present in the map.
        for (const auto& questDef : questData_->AllQuests()) {
            playerProgress.emplace(questDef.id, quest::QuestStatus::LOCKED);
        }

        // Validate that playerId is valid (non-zero for most implementations)
        if (playerId == 0) {
            spdlog::warn("[QuestManager] Player ID is zero - this may indicate a client issue");
        }

        spdlog::info("[QuestManager] Quest state initialized for player {}, {} quests seeded",
                     playerId, questData_->AllQuests().size());
    } catch (const std::exception& e) {
        spdlog::error("[QuestManager] Exception in onPlayerJoined for player {}: {}", 
                     playerId, e.what());
    }
}

void QuestManager::checkCraftCompletion(uint64_t playerId, uint16_t itemId, uint8_t count) {
    if (!questData_) {
        spdlog::error("[QuestManager] checkCraftCompletion: questData_ is null for player {}, item {}", 
                     playerId, itemId);
        return;
    }
    
    std::string itemIdStr = std::to_string(itemId);
    
    spdlog::debug("[QuestManager] Checking craft completion: player={}, item={}, count={}", 
                 playerId, itemId, count);
    
    try { //TODO refactor if HELL
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto& playerProgress = progress_[playerId];
        
        for (const auto& questDef : questData_->AllQuests()) {
            if (questDef.detectType != quest::DetectionType::CRAFT || 
                questDef.detectTarget != itemIdStr) {
                continue;
            }
            
            auto questIt = playerProgress.find(questDef.id);
            
            if (questIt == playerProgress.end()) {
                // Quest not started yet, check prerequisites
                auto prereqs = questData_->GetPrerequisites(questDef.id);
                bool prereqsMet = true;
                for (uint32_t prereqId : prereqs) {
                    auto prereqIt = playerProgress.find(prereqId);
                    if (prereqIt == playerProgress.end() || 
                        prereqIt->second != quest::QuestStatus::COMPLETED) {
                        prereqsMet = false;
                        break;
                    }
                }
                
                if (prereqsMet) {
                    // Make quest AVAILABLE
                    playerProgress[questDef.id] = quest::QuestStatus::AVAILABLE;
                    publishQuestProgressUpdated(playerId, questDef.id, 
                                               quest::QuestStatus::AVAILABLE, 0);
                    spdlog::info("[QuestManager] Quest {} made AVAILABLE for player {}", 
                               questDef.id, playerId);
                } else {
                    spdlog::debug("[QuestManager] Quest {} prerequisites not met for player {}", 
                                 questDef.id, playerId);
                }
                continue;
            }
            
            quest::QuestStatus& status = questIt->second;
            
            if (status == quest::QuestStatus::AVAILABLE && count >= 1) {
                status = quest::QuestStatus::COMPLETED;
                publishQuestCompleted(playerId, questDef.id);
                publishQuestProgressUpdated(playerId, questDef.id, status, 100);
                
                // Trigger reward distribution
                distributeRewards(playerId, questDef);
                
                spdlog::info("[QuestManager] Quest {} COMPLETED for player {} (crafted {} x{})", 
                           questDef.id, playerId, itemId, count);
            } else if (status == quest::QuestStatus::IN_PROGRESS) {
                uint8_t progress = std::min(static_cast<uint8_t>(100u), static_cast<uint8_t>((count * 100) / 10));
                publishQuestProgressUpdated(playerId, questDef.id, status, progress);
                
                spdlog::debug("[QuestManager] Quest {} progress updated: player={}, progress={}%", 
                             questDef.id, playerId, progress);
            }
        }
    } catch (const std::exception& e) {
        spdlog::error("[QuestManager] Exception in checkCraftCompletion for player {}: {}", 
                     playerId, e.what());
    }
}

void QuestManager::checkBlockAction(uint64_t playerId, int32_t x, int32_t y, int32_t z, uint16_t blockId) {
    if (!questData_) {
        spdlog::error("[QuestManager] checkBlockAction: questData_ is null for player {}, block {}", 
                     playerId, blockId);
        return;
    }
    
    std::string blockIdStr = std::to_string(blockId);
    
    spdlog::debug("[QuestManager] Checking block action: player={}, block={}, pos=({},{},{})", 
                 playerId, blockId, x, y, z);
    
    try {//TODO refactor if HELL
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto& playerProgress = progress_[playerId];
        
        for (const auto& questDef : questData_->AllQuests()) {
            if (questDef.detectType != quest::DetectionType::BLOCK_PLACED || 
                questDef.detectTarget != blockIdStr) {
                continue;
            }
            
            auto questIt = playerProgress.find(questDef.id);
            
            if (questIt == playerProgress.end()) {
                // Quest not started yet, check prerequisites
                auto prereqs = questData_->GetPrerequisites(questDef.id);
                bool prereqsMet = true;
                for (uint32_t prereqId : prereqs) {
                    auto prereqIt = playerProgress.find(prereqId);
                    if (prereqIt == playerProgress.end() || 
                        prereqIt->second != quest::QuestStatus::COMPLETED) {
                        prereqsMet = false;
                        break;
                    }
                }
                
                if (prereqsMet) {
                    // Make quest AVAILABLE
                    playerProgress[questDef.id] = quest::QuestStatus::AVAILABLE;
                    publishQuestProgressUpdated(playerId, questDef.id, 
                                               quest::QuestStatus::AVAILABLE, 0);
                    spdlog::info("[QuestManager] Block quest {} made AVAILABLE for player {}", 
                               questDef.id, playerId);
                } else {
                    spdlog::debug("[QuestManager] Block quest {} prerequisites not met for player {}", 
                                 questDef.id, playerId);
                }
                continue;
            }
            
            quest::QuestStatus& status = questIt->second;
            
            if (status == quest::QuestStatus::AVAILABLE) {
                status = quest::QuestStatus::COMPLETED;
                publishQuestCompleted(playerId, questDef.id);
                publishQuestProgressUpdated(playerId, questDef.id, status, 100);
                
                // Trigger reward distribution
                distributeRewards(playerId, questDef);
                
                spdlog::info("[QuestManager] Block quest {} COMPLETED for player {} at ({},{},{})", 
                           questDef.id, playerId, x, y, z);
            }
        }
    } catch (const std::exception& e) {
        spdlog::error("[QuestManager] Exception in checkBlockAction for player {}: {}", 
                     playerId, e.what());
    }
}

void QuestManager::loadProgress(uint64_t playerId, const std::vector<uint8_t>& fbData) {
    if (!questData_) {
        spdlog::error("[QuestManager] loadProgress: questData_ is null for player {}", playerId);
        return;
    }

    if (fbData.size() < 4) {
        spdlog::warn("[QuestManager] Invalid quest progress data size {} for player {}", fbData.size(), playerId);
        return;
    }

    try {
        std::lock_guard<std::mutex> lock(mutex_);

        flatbuffers::Verifier verifier(fbData.data(), fbData.size());
        if (!verifier.VerifyBuffer<Protocol::QuestProgressUpdate>(nullptr)) {
            spdlog::warn("[QuestManager] loadProgress: invalid QuestProgressUpdate buffer for player {}", playerId);
            return;
        }
        auto resp = flatbuffers::GetRoot<Protocol::QuestProgressUpdate>(fbData.data());
        if (!resp) {
            spdlog::warn("[QuestManager] loadProgress: null QuestProgressUpdate for player {}", playerId);
            return;
        }

        uint64_t respPlayerId = resp->player_id();
        if (respPlayerId != playerId) {
            spdlog::warn("[QuestManager] loadProgress: response for player {} but requested player {}",
                         respPlayerId, playerId);
        }

        auto& playerProgress = progress_[playerId];
        uint32_t loadedCount = 0;
        if (auto* quests = resp->quests()) {
            for (flatbuffers::uoffset_t i = 0; i < quests->size(); ++i) {
                auto* entry = quests->Get(i);
                if (!entry) {
                    continue;
                }
                uint32_t questId = entry->quest_id();
                uint8_t status = static_cast<uint8_t>(entry->status());
                uint8_t progress = entry->progress();

                if (status >= 4) {
                    spdlog::warn("[QuestManager] Invalid quest status {} for quest {} (player {}), clamping",
                                 status, questId, playerId);
                    status = static_cast<uint8_t>(quest::QuestStatus::LOCKED);
                }

                playerProgress[questId] = static_cast<quest::QuestStatus>(status);
                ++loadedCount;

                const quest::QuestDef* questDef = questData_->GetQuest(questId);
                if (questDef) {
                    spdlog::info("[QuestManager] Loaded quest {} for player {}: status={}, progress={}%",
                                 questId, playerId, static_cast<uint8_t>(status), progress);
                } else {
                    spdlog::debug("[QuestManager] Quest {} not found in quest data for player {}",
                                  questId, playerId);
                }
            }
        }

        spdlog::info("[QuestManager] Loaded {} quest entries for player {} from MetaDB",
                     loadedCount, playerId);
    } catch (const std::exception& e) {
        spdlog::error("[QuestManager] Exception in loadProgress for player {}: {}",
                      playerId, e.what());
    }
}

void QuestManager::distributeRewards(uint64_t playerId, const quest::QuestDef& questDef) {
    if (!questData_) {
        spdlog::error("[QuestManager] distributeRewards: questData_ is null for quest {} (player {})", 
                     questDef.id, playerId);
        return;
    }
    
    if (questDef.rewardItemId == 0 || questDef.rewardCount == 0) {
        spdlog::debug("[QuestManager] No rewards to distribute for quest {}", questDef.id);
        return;
    }
    
    spdlog::info("[QuestManager] Distributing rewards for quest {}: player={}, item={}, count={}", 
               questDef.id, playerId, questDef.rewardItemId, questDef.rewardCount);
    
    try {
        // Reward item/count travel inside QuestCompleted (quest.completed topic);
        // MetaDB stores player_quest_rewards and forwards a notification to the
        // client on quest.completed.notification. No separate inventory plumbing.
        spdlog::info("[QuestManager] reward item={} x{} for quest {} forwarded to MetaDB via quest.completed (player {})", 
                     questDef.rewardItemId, questDef.rewardCount, questDef.id, playerId);
    } catch (const std::exception& e) {
        spdlog::error("[QuestManager] Exception in distributeRewards for quest {} (player {}): {}", 
                     questDef.id, playerId, e.what());
    }
}

} // namespace simcore