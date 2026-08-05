package main

import (
	"encoding/csv"
	"log"
	"os"
	"strconv"
	"strings"
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

// packItemSpec converts a hierarchical item spec like "0:10:11:1" into the
// packed uint16 item id used across services. Port of ItemId::pack in
// src/common/ItemId.h: all segments before the last ':' are binary prefix
// bits (0/1), the last segment is a decimal payload.
func packItemSpec(s string) uint16 {
	if s == "" {
		return 0
	}
	lastColon := strings.LastIndexByte(s, ':')
	if lastColon == -1 {
		v, err := strconv.ParseUint(s, 10, 16)
		if err != nil {
			return 0
		}
		return uint16(v)
	}

	var prefix uint16
	plen := 0
	for i := 0; i < lastColon; i++ {
		c := s[i]
		if c == '0' {
			prefix = (prefix << 1) | 0
			plen++
		} else if c == '1' {
			prefix = (prefix << 1) | 1
			plen++
		}
	}
	if plen > 15 {
		return 0
	}

	payload, err := strconv.ParseUint(s[lastColon+1:], 10, 16)
	if err != nil {
		return 0
	}

	shift := 16 - plen
	return uint16((uint32(prefix) << shift) | uint32(payload))
}

// loadQuestDefinitions parses quests.csv and populates the global questDefs map.
// The CSV header is: id,title,description,era,section,prereqs,detect_type,detect_target,reward_item,reward_count,cost_item,cost_count,cooldown.
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
		if len(row) < 13 {
			log.Printf("[QUEST] loadQuestDefinitions: skipping malformed row %d (%d fields)", i, len(row))
			continue
		}
		id, err := strconv.ParseUint(row[0], 10, 32)
		if err != nil {
			log.Printf("[QUEST] loadQuestDefinitions: bad id in row %d: %v", i, err)
			continue
		}
		rewardItem := packItemSpec(row[8])
		rewardCount, err := strconv.ParseUint(row[9], 10, 8)
		if err != nil {
			log.Printf("[QUEST] loadQuestDefinitions: bad reward_count in row %d: %v", i, err)
			continue
		}
		costItem := packItemSpec(row[10])
		costCount, err := strconv.ParseUint(row[11], 10, 8)
		if err != nil {
			log.Printf("[QUEST] loadQuestDefinitions: bad cost_count in row %d: %v", i, err)
			continue
		}
		cooldown, err := strconv.ParseUint(row[12], 10, 16)
		if err != nil {
			log.Printf("[QUEST] loadQuestDefinitions: bad cooldown in row %d: %v", i, err)
			continue
		}
		defs[uint32(id)] = QuestDef{
			ID:           uint32(id),
			Title:        row[1],
			RewardItemID: rewardItem,
			RewardCount:  uint8(rewardCount),
			Era:          eraValues[row[3]],
			Section:      row[4],
			DetectType:   row[6],
			CostItemID:   costItem,
			CostCount:    uint8(costCount),
			CooldownSecs: uint16(cooldown),
		}
	}

	questDefs = defs
	log.Printf("[QUEST] loaded %d quest definitions from %s", len(defs), csvPath)
	return nil
}
