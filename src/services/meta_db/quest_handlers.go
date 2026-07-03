package main

import (
	"fmt"
	"log"
	"time"

	flatbuffers "github.com/google/flatbuffers/go"
	"github.com/gtnh-platform/protocol/generated/go/Protocol"
)

// HandleQuestGet handles QuestProgressUpdate requests from clients.
// Reads the requested player_id and returns their quest progress.
func HandleQuestGet(topic string, payload []byte, m *MetaDB) {
	if len(payload) == 0 {
		log.Printf("[QUEST] HandleQuestGet: empty payload for topic %s", topic)
		return
	}

	req := Protocol.GetRootAsQuestProgressUpdate(payload, 0)
	if req == nil {
		log.Printf("[QUEST] HandleQuestGet: failed to parse QuestProgressUpdate")
		return
	}

	playerID := req.PlayerId()
	log.Printf("[QUEST] HandleQuestGet: player=%d", playerID)

	progress, err := GetQuestProgress(m.db, playerID)
	if err != nil {
		log.Printf("[QUEST] HandleQuestGet: failed to get quest progress for player %d: %v", playerID, err)
		return
	}

	builder := flatbuffers.NewBuilder(1024)
	questsOffsets := make([]flatbuffers.UOffsetT, len(progress))

	for i, qp := range progress {
		Protocol.QuestEntryStart(builder)
		Protocol.QuestEntryAddQuestId(builder, qp.QuestID)
		Protocol.QuestEntryAddStatus(builder, Protocol.QuestStatus(qp.Status))
		Protocol.QuestEntryAddProgress(builder, qp.ProgressPercent)
		questsOffsets[i] = Protocol.QuestEntryEnd(builder)
	}

	Protocol.QuestProgressUpdateStartQuestsVector(builder, len(questsOffsets))
	for i := len(questsOffsets) - 1; i >= 0; i-- {
		builder.PrependUOffsetT(questsOffsets[i])
	}
	questsVec := builder.EndVector(len(questsOffsets))

	Protocol.QuestProgressUpdateStart(builder)
	Protocol.QuestProgressUpdateAddPlayerId(builder, playerID)
	Protocol.QuestProgressUpdateAddQuests(builder, questsVec)
	respOffset := Protocol.QuestProgressUpdateEnd(builder)
	builder.Finish(respOffset)

	respBytes := builder.FinishedBytes()
	handlerTopic := topic + ".response"
	if m.rc != nil {
		log.Printf("[QUEST] HandleQuestGet: publishing response to %s (%d bytes)", handlerTopic, len(respBytes))
		m.rc.PublishRaw(handlerTopic, respBytes)
	} else {
		log.Printf("[QUEST] HandleQuestGet: no router client, skipping publish")
	}
}

// HandleQuestSet handles QuestProgressUpdate requests to update quest progress.
// Reads the payload, upserts into DB, and publishes to quest.progress.updated topic.
func HandleQuestSet(topic string, payload []byte, m *MetaDB) {
	if len(payload) == 0 {
		log.Printf("[QUEST] HandleQuestSet: empty payload for topic %s", topic)
		return
	}

	req := Protocol.GetRootAsQuestProgressUpdate(payload, 0)
	if req == nil {
		log.Printf("[QUEST] HandleQuestSet: failed to parse QuestProgressUpdate")
		return
	}

	playerID := req.PlayerId()
	quests := make([]QuestProgress, req.QuestsLength())
	for i := 0; i < req.QuestsLength(); i++ {
		entry := new(Protocol.QuestEntry)
		if req.Quests(entry, i) {
			quests[i] = QuestProgress{
				PlayerID:      playerID,
				QuestID:       entry.QuestId(),
				Status:        uint8(entry.Status()),
				ProgressPercent: entry.Progress(),
			}
		}
	}

	log.Printf("[QUEST] HandleQuestSet: player=%d, %d quests to update", playerID, len(quests))

	err := SetQuestProgressBatch(m.db, playerID, quests)
	if err != nil {
		log.Printf("[QUEST] HandleQuestSet: failed to update quest progress for player %d: %v", playerID, err)
		return
	}

	// Build QuestCompleted event if any quest was completed
	for _, qp := range quests {
		if qp.Status == uint8(Protocol.QuestStatusCOMPLETED) {
			builder := flatbuffers.NewBuilder(64)
			Protocol.QuestCompletedStart(builder)
			Protocol.QuestCompletedAddPlayerId(builder, playerID)
			Protocol.QuestCompletedAddQuestId(builder, qp.QuestID)
			Protocol.QuestCompletedAddTimestamp(builder, uint64(time.Now().UnixNano()))
			completedOffset := Protocol.QuestCompletedEnd(builder)
			builder.Finish(completedOffset)
			completedBytes := builder.FinishedBytes()

			log.Printf("[QUEST] HandleQuestSet: publishing quest.completed for player %d quest %d", playerID, qp.QuestID)
			if m.rc != nil {
				m.rc.PublishRaw("quest.completed", completedBytes)
			}
		}
	}

	// Publish updated progress to quest.progress.updated topic
	builder := flatbuffers.NewBuilder(1024)
	questsOffsets := make([]flatbuffers.UOffsetT, len(quests))
	for i, qp := range quests {
		Protocol.QuestEntryStart(builder)
		Protocol.QuestEntryAddQuestId(builder, qp.QuestID)
		Protocol.QuestEntryAddStatus(builder, Protocol.QuestStatus(qp.Status))
		Protocol.QuestEntryAddProgress(builder, qp.ProgressPercent)
		questsOffsets[i] = Protocol.QuestEntryEnd(builder)
	}

	Protocol.QuestProgressUpdateStartQuestsVector(builder, len(questsOffsets))
	for i := len(questsOffsets) - 1; i >= 0; i-- {
		builder.PrependUOffsetT(questsOffsets[i])
	}
	questsVec := builder.EndVector(len(questsOffsets))

	Protocol.QuestProgressUpdateStart(builder)
	Protocol.QuestProgressUpdateAddPlayerId(builder, playerID)
	Protocol.QuestProgressUpdateAddQuests(builder, questsVec)
	updateOffset := Protocol.QuestProgressUpdateEnd(builder)
	builder.Finish(updateOffset)

	updateBytes := builder.FinishedBytes()
	log.Printf("[QUEST] HandleQuestSet: publishing quest.progress.updated to gateway (%d bytes)", len(updateBytes))
	if m.rc != nil {
		m.rc.PublishRaw("quest.progress.updated", updateBytes)
	}
}

// HandleQuestCompleted handles QuestCompleted events from SimulationCore.
// Forwards the completion event to Gateway via quest.completed topic, and also stores reward information in player_quest_rewards table.
func HandleQuestCompleted(topic string, payload []byte, m *MetaDB) {
	if len(payload) == 0 {
		log.Printf("[QUEST] HandleQuestCompleted: empty payload for topic %s", topic)
		return
	}

	completed := Protocol.GetRootAsQuestCompleted(payload, 0)
	if completed == nil {
		log.Printf("[QUEST] HandleQuestCompleted: failed to parse QuestCompleted")
		return
	}

	playerID := completed.PlayerId()
	questID := completed.QuestId()
	log.Printf("[QUEST] HandleQuestCompleted: player=%d quest=%d", playerID, questID)

	// Get quest definition to determine rewards
	questDef := GetQuestDefinition(questID)
	var rewardItemID uint16 = 0
	var rewardCount uint8 = 0
	
	if questDef != nil {
		rewardItemID = questDef.RewardItemID
		rewardCount = questDef.RewardCount
		log.Printf("[QUEST] HandleQuestCompleted: quest=%d rewards item=%d count=%d", questID, rewardItemID, rewardCount)
	} else {
		log.Printf("[QUEST] HandleQuestCompleted: WARNING - no quest definition found for quest=%d", questID)
	}

	// Store reward in player_quest_rewards table
	rewardType := "item"
	rewardValue := float64(0)
	timestamp := time.Now().Unix()
	metadata := fmt.Sprintf("quest_id=%d,era=%d,section=%s", questID, questDef.Era, questDef.Section)

	err := StorePlayerQuestReward(m.db, playerID, questID, rewardType, rewardItemID, rewardCount, rewardValue, 0, timestamp, metadata)
	if err != nil {
		log.Printf("[QUEST] HandleQuestCompleted: failed to store reward for player=%d quest=%d: %v", playerID, questID, err)
	} else {
		log.Printf("[QUEST] HandleQuestCompleted: reward stored successfully for player=%d quest=%d", playerID, questID)
	}

	// Publish to quest.completed topic for Gateway with reward data
	builder := flatbuffers.NewBuilder(64)
	Protocol.QuestCompletedNotificationStart(builder)
	Protocol.QuestCompletedNotificationAddPlayerId(builder, playerID)
	Protocol.QuestCompletedNotificationAddQuestId(builder, questID)
	Protocol.QuestCompletedNotificationAddRewardItemId(builder, rewardItemID)
	Protocol.QuestCompletedNotificationAddRewardCount(builder, rewardCount)
	notificationOffset := Protocol.QuestCompletedNotificationEnd(builder)
	builder.Finish(notificationOffset)

	notificationBytes := builder.FinishedBytes()
	log.Printf("[QUEST] HandleQuestCompleted: publishing QuestCompletedNotification to quest.completed (%d bytes)", len(notificationBytes))
	if m.rc != nil {
		m.rc.PublishRaw("quest.completed", notificationBytes)
	}
}