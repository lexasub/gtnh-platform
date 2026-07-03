#include "QuestManager.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <stdexcept>
#include <mutex>
#include <cstdint>

namespace simcore {

void QuestManager::onPlayerJoined(uint64_t playerId) {
    if (!questData_) {
        spdlog::error("[QuestManager] onPlayerJoined: questData_ is null for player {}", playerId);
        return;
    }
    
    spdlog::info("[QuestManager] Player {} joined, initializing quest state", playerId);
    
    try {
        // Initialize player quest state with thread safety
        std::lock_guard<std::mutex> lock(mutex_);
        progress_[playerId] = std::unordered_map<uint32_t, quest::QuestStatus>();
        
        // Validate that playerId is valid (non-zero for most implementations)
        if (playerId == 0) {
            spdlog::warn("[QuestManager] Player ID is zero - this may indicate a client issue");
        }
        
        spdlog::info("[QuestManager] Quest state initialized for player {}", playerId);
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
    
    if (fbData.size() < 12) {
        spdlog::warn("[QuestManager] Invalid quest progress data size for player {}", playerId);
        return;
    }
    
    spdlog::debug("[QuestManager] Loading quest progress for player {} from {} bytes", 
                 playerId, fbData.size());
    
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        
        size_t offset = 0;
        uint32_t loadedCount = 0;

        //TODO - use const offsets
        while (offset + 12 <= fbData.size()) {
            uint64_t pId = *reinterpret_cast<const uint64_t*>(&fbData[offset]);
            offset += 8;
            
            uint32_t questId = *reinterpret_cast<const uint32_t*>(&fbData[offset]);
            offset += 4;
            
            uint8_t status = fbData[offset];
            offset += 1;
            
            uint8_t progress = fbData[offset];
            offset += 1;
            
            if (pId == playerId) {
                // Validate status value
                if (status >= 4) {  // MAX_STATUS = 3 (COMPLETED), so anything >= 4 is invalid
                    spdlog::warn("[QuestManager] Invalid quest status {} for quest {} (player {}), clamping", 
                                status, questId, playerId);
                    status = static_cast<uint8_t>(quest::QuestStatus::LOCKED);
                }
                
                progress_[playerId][questId] = static_cast<quest::QuestStatus>(status);
                loadedCount++;
                
                const quest::QuestDef* questDef = questData_->GetQuest(questId);
                if (questDef) {
                    spdlog::info("[QuestManager] Loaded quest {} for player {}: status={}, progress={}%)", 
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
        // TODO: Integrate with inventory system to add reward items
        // This would typically involve:
        // 1. Publishing to inventory service via gateway
        // 2. Or calling inventory manager directly
        // 3. Publishing quest reward events
        
        // For now, log the reward that would be distributed
        spdlog::info("[QuestManager] Would distribute reward: itemId={}, count={} to player {}", 
                   questDef.rewardItemId, questDef.rewardCount, playerId);
        
        // Publish quest reward event for gateway
        std::vector<uint8_t> rewardData;
        rewardData.reserve(20);
        
        rewardData.insert(rewardData.end(),
                          reinterpret_cast<const uint8_t*>(&playerId),
                          reinterpret_cast<const uint8_t*>(&playerId) + 8);
        
        rewardData.insert(rewardData.end(),
                          reinterpret_cast<const uint8_t*>(&questDef.rewardItemId),
                          reinterpret_cast<const uint8_t*>(&questDef.rewardItemId) + 2);
        
        rewardData.push_back(questDef.rewardCount);
        
        publishCallback_("quest.reward.distributed", rewardData.data(), rewardData.size());
        
        spdlog::info("[QuestManager] Reward distribution event published for quest {} (player {})", 
                   questDef.id, playerId);
    } catch (const std::exception& e) {
        spdlog::error("[QuestManager] Exception in distributeRewards for quest {} (player {}): {}", 
                     questDef.id, playerId, e.what());
    }
}

} // namespace simcore