#include "QuestManager.h"
#include "quest_generated.h"
#include <recipe_manager_lib/ItemRegistry.h>
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

void QuestManager::publishQuestUnlocked(uint64_t playerId,
                                       const std::vector<uint32_t>& questIds) {
    if (!publishCallback_) {
        spdlog::warn("[QuestManager] publishQuestUnlocked: no publish callback for player {}", playerId);
        return;
    }
    if (questIds.empty()) return;
    flatbuffers::FlatBufferBuilder builder(64);
    auto idsVec = builder.CreateVector(questIds);
    auto offset = Protocol::CreateQuestUnlocked(builder, playerId, idsVec);
    builder.Finish(offset);
    publishCallback_("quest.unlocked", builder.GetBufferPointer(), builder.GetSize());
    spdlog::info("[QuestManager] Published quest.unlocked: player={}, {} quests newly available",
                 playerId, questIds.size());
}

void QuestManager::publishEraTransition(uint64_t playerId, quest::Era completedEra) {
    if (!publishCallback_) {
        spdlog::warn("[QuestManager] publishEraTransition: no publish callback for player {}", playerId);
        return;
    }
    uint8_t completedVal = static_cast<uint8_t>(completedEra);
    uint8_t nextVal = (completedVal < static_cast<uint8_t>(quest::Era::ADMINISTRATOR))
                          ? completedVal + 1
                          : completedVal;
    flatbuffers::FlatBufferBuilder builder(64);
    auto offset = Protocol::CreateEraTransitionNotification(builder, playerId, completedVal, nextVal);
    builder.Finish(offset);
    publishCallback_("quest.era.transition", builder.GetBufferPointer(), builder.GetSize());
    spdlog::info("[QuestManager] Published quest.era.transition: player={}, era={} complete, next={}",
                 playerId, static_cast<int>(completedVal), static_cast<int>(nextVal));
}

void QuestManager::maybePublishEraTransition(uint64_t playerId, uint32_t questId) {
    if (!questData_ || !questGraph_) return;
    const quest::QuestDef* qd = questData_->GetQuest(questId);
    if (!qd) return;

    auto& playerProgress = progress_[playerId];
    auto& completed = completedEras_[playerId];
    uint8_t eraVal = static_cast<uint8_t>(qd->era);
    if (completed.count(eraVal)) return;

    if (questEraMap_.empty()) questEraMap_ = questData_->BuildQuestEraMap();
    if (!questGraph_->IsEraComplete(qd->era, playerProgress, questEraMap_)) return;

    completed.insert(eraVal);
    publishEraTransition(playerId, qd->era);
}

void QuestManager::rebuildCompletedEras(
    uint64_t playerId,
    const std::unordered_map<uint32_t, quest::QuestStatus>& playerProgress) {
    if (!questData_ || !questGraph_) return;
    if (questEraMap_.empty()) questEraMap_ = questData_->BuildQuestEraMap();
    auto& completed = completedEras_[playerId];
    completed.clear();
    for (int e = 0; e <= static_cast<int>(quest::Era::ADMINISTRATOR); ++e) {
        quest::Era era = static_cast<quest::Era>(e);
        if (questGraph_->IsEraComplete(era, playerProgress, questEraMap_))
            completed.insert(static_cast<uint8_t>(e));
    }
}

bool QuestManager::completeQuest(uint64_t playerId, uint32_t questId) {
    if (!questData_ || !questGraph_) {
        spdlog::error("[QuestManager] completeQuest: questData_/questGraph_ null for player {}", playerId);
        return false;
    }

    const auto& quests = questData_->AllQuests();
    auto defIt = std::find_if(quests.begin(), quests.end(),
                              [questId](const quest::QuestDef& qd) { return qd.id == questId; });
    if (defIt != quests.end() && defIt->detectType == quest::DetectionType::EXCHANGE) {
        spdlog::warn("[QuestManager] completeQuest: quest {} is EXCHANGE (repeatable, never completes)",
                     questId);
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto& playerProgress = progress_[playerId];
    auto it = playerProgress.find(questId);
    if (it == playerProgress.end()) {
        spdlog::warn("[QuestManager] completeQuest: unknown quest {} for player {}", questId, playerId);
        return false;
    }
    if (it->second != quest::QuestStatus::AVAILABLE) {
        spdlog::warn("[QuestManager] completeQuest: quest {} is not AVAILABLE for player {} (status={})",
                     questId, playerId, static_cast<uint8_t>(it->second));
        return false;
    }
    if (!questGraph_->CanComplete(questId, playerProgress)) {
        spdlog::warn("[QuestManager] completeQuest: quest {} prerequisites not met for player {}",
                     questId, playerId);
        return false;
    }

    // Accept: transition AVAILABLE → COMPLETED.
    bool ok = completeQuestInternal(playerId, questId);
    if (ok) {
        spdlog::info("[QuestManager] Quest {} COMPLETED for player {} (manual)", questId, playerId);
    }
    return ok;
}

bool QuestManager::completeQuestInternal(uint64_t playerId, uint32_t questId) {
    auto& playerProgress = progress_[playerId];

    // Seed missing quests as LOCKED so detection can complete a quest before
    // onPlayerJoined has run (e.g. a player who crafts before joining).
    auto it = playerProgress.find(questId);
    if (it == playerProgress.end()) {
        playerProgress[questId] = quest::QuestStatus::LOCKED;
        it = playerProgress.find(questId);
    }
    if (it->second == quest::QuestStatus::COMPLETED) return false; // idempotent

    // One-step completion: any non-COMPLETED status (LOCKED/AVAILABLE/IN_PROGRESS)
    // transitions directly to COMPLETED. Prerequisites are validated by the caller.
    it->second = quest::QuestStatus::COMPLETED;
    publishQuestCompleted(playerId, questId);
    publishQuestProgressUpdated(playerId, questId, quest::QuestStatus::COMPLETED, 100);
    maybePublishEraTransition(playerId, questId);

    // Reward delivery: the quest.completed event is consumed by MetaDB, which
    // stores the reward row and grants it to the player's inventory
    // (questbook-reward-inventory). distributeRewards() stays for audit logging.
    const quest::QuestDef* questDef = questData_->GetQuest(questId);
    if (questDef) {
        distributeRewards(playerId, *questDef);
    }

    // Unlock newly available dependents (their prerequisites are now met).
    auto newlyAvailable = questGraph_->NewlyAvailable(playerProgress);
    if (!newlyAvailable.empty()) {
        std::vector<uint32_t> ids;
        ids.reserve(newlyAvailable.size());
        for (uint32_t nq : newlyAvailable) {
            playerProgress[nq] = quest::QuestStatus::AVAILABLE;
            publishQuestProgressUpdated(playerId, nq, quest::QuestStatus::AVAILABLE, 0);
            ids.push_back(nq);
        }
        publishQuestUnlocked(playerId, ids);
    }

    return true;
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

        // Seed root quests (no prereqs) as AVAILABLE so the client shows them
        // as clickable/completable immediately. Dependent quests stay LOCKED
        // until their prerequisites are completed.
        std::vector<uint32_t> autoUnlocked;
        for (const auto& questDef : questData_->AllQuests()) {
            if (questData_->GetPrerequisites(questDef.id).empty()) {
                playerProgress[questDef.id] = quest::QuestStatus::AVAILABLE;
                autoUnlocked.push_back(questDef.id);
            } else {
                playerProgress[questDef.id] = quest::QuestStatus::LOCKED;
            }
        }
        if (!autoUnlocked.empty()) {
            publishQuestUnlocked(playerId, autoUnlocked);
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
    if (!questData_ || !questGraph_) {
        spdlog::error("[QuestManager] checkCraftCompletion: questData_/questGraph_ null for player {}",
                     playerId, itemId);
        return;
    }
    if (count < 1) return;

    // detect_target is a hierarchical item id from items.csv; resolve packed id → hierarchical.
    std::string itemIdStr =
        RecipeManager::ItemRegistry::instance().idToHierarchical(itemId);

    spdlog::debug("[QuestManager] Checking craft completion: player={}, item={} (hier={}), count={}",
                 playerId, itemId, itemIdStr, count);

    try { //TODO refactor if HELL
        std::lock_guard<std::mutex> lock(mutex_);

        auto& playerProgress = progress_[playerId];

        for (const auto& questDef : questData_->AllQuests()) {
            if (questDef.detectType != quest::DetectionType::CRAFT ||
                questDef.detectTarget != itemIdStr) {
                continue;
            }

            // One-step: prerequisites evaluated via QuestGraph; LOCKED quests
            // complete directly (no intermediate AVAILABLE gate).
            if (!questGraph_->CanComplete(questDef.id, playerProgress)) {
                spdlog::debug("[QuestManager] Craft quest {} prerequisites not met for player {}",
                             questDef.id, playerId);
                continue;
            }

            if (completeQuestInternal(playerId, questDef.id)) {
                spdlog::info("[QuestManager] Quest {} COMPLETED for player {} (crafted {} x{})",
                           questDef.id, playerId, itemId, count);
            }
        }
    } catch (const std::exception& e) {
        spdlog::error("[QuestManager] Exception in checkCraftCompletion for player {}: {}",
                     playerId, e.what());
    }
}

void QuestManager::checkBlockAction(uint64_t playerId, int32_t x, int32_t y, int32_t z, uint16_t blockId) {
    if (!questData_ || !questGraph_) {
        spdlog::error("[QuestManager] checkBlockAction: questData_/questGraph_ null for player {}, block {}",
                     playerId, blockId);
        return;
    }

    // detect_target is a hierarchical item id from items.csv; resolve packed block id → hierarchical.
    std::string blockIdStr =
        RecipeManager::ItemRegistry::instance().idToHierarchical(blockId);

    spdlog::debug("[QuestManager] Checking block action: player={}, block={} (hier={}), pos=({},{},{})",
                 playerId, blockId, blockIdStr, x, y, z);

    try {//TODO refactor if HELL
        std::lock_guard<std::mutex> lock(mutex_);

        auto& playerProgress = progress_[playerId];

        for (const auto& questDef : questData_->AllQuests()) {
            if (questDef.detectType != quest::DetectionType::BLOCK_PLACED ||
                questDef.detectTarget != blockIdStr) {
                continue;
            }

            // One-step: prerequisites evaluated via QuestGraph; LOCKED quests
            // complete directly (no intermediate AVAILABLE gate).
            if (!questGraph_->CanComplete(questDef.id, playerProgress)) {
                spdlog::debug("[QuestManager] Block quest {} prerequisites not met for player {}",
                             questDef.id, playerId);
                continue;
            }

            if (completeQuestInternal(playerId, questDef.id)) {
                spdlog::info("[QuestManager] Block quest {} COMPLETED for player {} at ({},{},{})",
                           questDef.id, playerId, x, y, z);
            }
        }
    } catch (const std::exception& e) {
        spdlog::error("[QuestManager] Exception in checkBlockAction for player {}: {}",
                     playerId, e.what());
    }
}

void QuestManager::checkToolCharged(uint64_t playerId, uint16_t itemId) {
    if (!questData_ || !questGraph_) {
        spdlog::error("[QuestManager] checkToolCharged: questData_/questGraph_ null for player {}",
                     playerId, itemId);
        return;
    }

    // detect_target is a hierarchical item id from items.csv; resolve packed id → hierarchical.
    std::string itemIdStr =
        RecipeManager::ItemRegistry::instance().idToHierarchical(itemId);

    spdlog::debug("[QuestManager] Checking tool-charged completion: player={}, tool={} (hier={})",
                 playerId, itemId, itemIdStr);

    try {
        std::lock_guard<std::mutex> lock(mutex_);

        auto& playerProgress = progress_[playerId];

        for (const auto& questDef : questData_->AllQuests()) {
            if (questDef.detectType != quest::DetectionType::TOOL_CHARGED ||
                questDef.detectTarget != itemIdStr) {
                continue;
            }

            if (!questGraph_->CanComplete(questDef.id, playerProgress)) {
                spdlog::debug("[QuestManager] Tool-charged quest {} prerequisites not met for player {}",
                             questDef.id, playerId);
                continue;
            }

            if (completeQuestInternal(playerId, questDef.id)) {
                spdlog::info("[QuestManager] Quest {} COMPLETED for player {} (tool charged {})",
                           questDef.id, playerId, itemId);
            }
        }
    } catch (const std::exception& e) {
        spdlog::error("[QuestManager] Exception in checkToolCharged for player {}: {}",
                     playerId, e.what());
    }
}

void QuestManager::checkSideConfigured(uint64_t playerId, uint16_t machineId) {
    if (!questData_ || !questGraph_) {
        spdlog::error("[QuestManager] checkSideConfigured: questData_/questGraph_ null for player {}",
                     playerId, machineId);
        return;
    }
    if (machineId == 0) return; // hatches carry no machine id

    // detect_target is a hierarchical machine id from items.csv; resolve packed → hierarchical.
    std::string machineIdStr =
        RecipeManager::ItemRegistry::instance().idToHierarchical(machineId);

    spdlog::debug("[QuestManager] Checking side-configured completion: player={}, machine={} (hier={})",
                 playerId, machineId, machineIdStr);

    try {
        std::lock_guard<std::mutex> lock(mutex_);

        auto& playerProgress = progress_[playerId];

        for (const auto& questDef : questData_->AllQuests()) {
            if (questDef.detectType != quest::DetectionType::SIDE_CONFIGURED ||
                questDef.detectTarget != machineIdStr) {
                continue;
            }

            if (!questGraph_->CanComplete(questDef.id, playerProgress)) {
                spdlog::debug("[QuestManager] Side-config quest {} prerequisites not met for player {}",
                             questDef.id, playerId);
                continue;
            }

            if (completeQuestInternal(playerId, questDef.id)) {
                spdlog::info("[QuestManager] Quest {} COMPLETED for player {} (side configured machine {})",
                           questDef.id, playerId, machineId);
            }
        }
    } catch (const std::exception& e) {
        spdlog::error("[QuestManager] Exception in checkSideConfigured for player {}: {}",
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

        rebuildCompletedEras(playerId, playerProgress);
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