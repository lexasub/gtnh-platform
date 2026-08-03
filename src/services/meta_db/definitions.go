package main

import (
	"encoding/csv"
	"log"
	"os"
	"strconv"
)

// questDefs is the quest definition map loaded from quests.csv at startup.
// Guarded by the fact that it is written once during init and read-only after.
var questDefs map[uint32]QuestDef

// eraValues maps the era name in quests.csv to its numeric representation.
var eraValues = map[string]uint8{
	"vagrant":       0,
	"apprentice":    1,
	"expert":        2,
	"administrator": 3,
}

// loadQuestDefinitions parses quests.csv and populates the global questDefs map.
// The CSV header is: id,title,description,era,section,prereqs,detect_type,detect_target,reward_item,reward_count.
// Malformed rows are skipped with a warning; the function returns the open/parse error otherwise.
func loadQuestDefinitions(csvPath string) error {
	f, err := os.Open(csvPath)
	if err != nil {
		return err
	}
	defer f.Close()

	reader := csv.NewReader(f)
	records, err := reader.ReadAll()
	if err != nil {
		return err
	}

	defs := make(map[uint32]QuestDef, len(records))
	for i, row := range records {
		if i == 0 {
			continue // header row
		}
		if len(row) < 10 {
			log.Printf("[QUEST] loadQuestDefinitions: skipping malformed row %d (%d fields)", i, len(row))
			continue
		}
		id, err := strconv.ParseUint(row[0], 10, 32)
		if err != nil {
			log.Printf("[QUEST] loadQuestDefinitions: bad id in row %d: %v", i, err)
			continue
		}
		rewardItem, err := strconv.ParseUint(row[8], 10, 16)
		if err != nil {
			log.Printf("[QUEST] loadQuestDefinitions: bad reward_item in row %d: %v", i, err)
			continue
		}
		rewardCount, err := strconv.ParseUint(row[9], 10, 8)
		if err != nil {
			log.Printf("[QUEST] loadQuestDefinitions: bad reward_count in row %d: %v", i, err)
			continue
		}
		defs[uint32(id)] = QuestDef{
			ID:           uint32(id),
			Title:        row[1],
			RewardItemID: uint16(rewardItem),
			RewardCount:  uint8(rewardCount),
			Era:          eraValues[row[3]],
			Section:      row[4],
		}
	}

	questDefs = defs
	log.Printf("[QUEST] loaded %d quest definitions from %s", len(defs), csvPath)
	return nil
}
