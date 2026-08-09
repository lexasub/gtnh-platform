package main

import (
	"encoding/csv"
	"encoding/json"
	"fmt"
	"log"
	"os"
	"path/filepath"
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

// loadQuestDefinitions parses the normalized quest data files and populates
// the global questDefs map.
//
// Data sources (all under data/quests):
//   - quests.csv — 9 columns:
//     id,title,description,era,section,cost_item,cost_count,cooldown,target_count.
//     detect_type/detect_target/reward_item/reward_count/cooldown-as-column-11
//     were dropped; detection + rewards now live in the JSON files below.
//   - quest_requirements.json — quest id -> { auto_complete, requirements[] };
//     requirement kind merges back into DetectType/TargetItemID/TargetCount so
//     downstream MetaDB grant/notification handlers keep working.
//   - quest_rewards.json — quest id -> { rewards[] | choice_of[] }; the first
//     item reward populates RewardItemID/RewardCount (MetaDB's single-reward
//     grant path).
//
// Malformed rows are skipped with a warning; the function returns the
// open/parse error otherwise.
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

	reqs, err := loadQuestRequirementsJSON(questDataDir(csvPath))
	if err != nil {
		return err
	}
	rewards, err := loadQuestRewardsJSON(questDataDir(csvPath))
	if err != nil {
		return err
	}

	defs := make(map[uint32]QuestDef, len(records))
	for i, row := range records {
		if i == 0 {
			continue // header row
		}
		if len(row) < 9 {
			log.Printf("[QUEST] loadQuestDefinitions: skipping malformed row %d (%d fields)", i, len(row))
			continue
		}
		id, err := strconv.ParseUint(row[0], 10, 32)
		if err != nil {
			log.Printf("[QUEST] loadQuestDefinitions: bad id in row %d: %v", i, err)
			continue
		}
		costItem := packItemSpec(row[5])
		costCount := parseUintField(row[6])
		cooldown := parseUintField(row[7])
		targetCount := parseUintField(row[8])

		qid := uint32(id)
		def := QuestDef{
			ID:           qid,
			Title:        row[1],
			Era:          eraValues[row[3]],
			Section:      row[4],
			CostItemID:   costItem,
			CostCount:    uint8(costCount),
			CooldownSecs: uint16(cooldown),
		}

		// Merge JSON requirement objective back into quest def fields so
		// MetaDB's existing detection/grant code keeps working.
		if qr, ok := reqs[qid]; ok {
			def.AutoComplete = qr.AutoComplete
			if len(qr.Requirements) > 0 {
				goal := qr.Requirements[0]
				def.DetectType = reqKindToDetectType(goal.Kind)
				def.TargetItemID = packItemSpec(goal.Item)
				if goal.Count > 0 {
					def.TargetCount = goal.Count
				} else {
					def.TargetCount = uint16(targetCount)
				}
			}
		} else {
			def.TargetCount = uint16(targetCount)
		}

		// Attach the first item reward (MetaDB grants a single reward per quest).
		if rw, ok := rewards[qid]; ok {
			if w := rw.firstItem(); w != nil {
				def.RewardItemID = packItemSpec(w.Item)
				def.RewardCount = uint8(w.Count)
			}
		}

		defs[qid] = def
	}

	questDefs = defs
	log.Printf("[QUEST] loaded %d quest definitions from %s", len(defs), csvPath)
	return nil
}

// questDataDir returns the directory that contains the quest CSV, so relative
// JSON paths resolve consistently regardless of the process working directory.
func questDataDir(csvPath string) string {
	if dir := filepath.Dir(csvPath); dir != "" && dir != "." {
		return dir
	}
	return "data/quests"
}

// parseUintField parses a CSV numeric cell, treating empty/unparseable values
// as 0 (cost/cooled columns are blank for most quests).
func parseUintField(s string) uint64 {
	if s == "" {
		return 0
	}
	v, err := strconv.ParseUint(s, 10, 32)
	if err != nil {
		return 0
	}
	return v
}

// questRequirements is the top-level shape of quest_requirements.json:
// quest id -> { auto_complete, requirements[] }.
type questRequirements map[uint32]questRequirementGroup

type questRequirementGroup struct {
	AutoComplete bool               `json:"auto_complete"`
	Requirements []questRequirement `json:"requirements"`
}

type questRequirement struct {
	Kind    string `json:"kind"`
	Item    string `json:"item"`
	Count   uint16 `json:"count"`
	Consume bool   `json:"consume"`
	Machine string `json:"machine"`
}

func loadQuestRequirementsJSON(dir string) (questRequirements, error) {
	raw, err := os.ReadFile(filepath.Join(dir, "quest_requirements.json"))
	if err != nil {
		return nil, err
	}
	out := make(questRequirements)
	if err := json.Unmarshal(raw, &out); err != nil {
		return nil, fmt.Errorf("quest_requirements.json: %w", err)
	}
	return out, nil
}

// questRewards is the top-level shape of quest_rewards.json:
// quest id -> { rewards[] | choice_of[] }.
type questRewards map[uint32]questRewardGroup

type questRewardGroup struct {
	Rewards  []questRewardEntry `json:"rewards"`
	ChoiceOf []questRewardEntry `json:"choice_of"`
}

type questRewardEntry struct {
	Type  string `json:"type"`
	Item  string `json:"item"`
	Count uint16 `json:"count"`
	Value float64 `json:"value"`
}

// firstItem returns the first item-typed reward across rewards, then
// choice_of, or nil if the quest grants no item reward.
func (g *questRewardGroup) firstItem() *questRewardEntry {
	for i := range g.Rewards {
		if g.Rewards[i].Type == "item" {
			return &g.Rewards[i]
		}
	}
	for i := range g.ChoiceOf {
		if g.ChoiceOf[i].Type == "item" {
			return &g.ChoiceOf[i]
		}
	}
	return nil
}

func loadQuestRewardsJSON(dir string) (questRewards, error) {
	raw, err := os.ReadFile(filepath.Join(dir, "quest_rewards.json"))
	if err != nil {
		return nil, err
	}
	out := make(questRewards)
	if err := json.Unmarshal(raw, &out); err != nil {
		return nil, fmt.Errorf("quest_rewards.json: %w", err)
	}
	return out, nil
}

// reqKindToDetectType maps the JSON requirement kind to the legacy MetaDB
// DetectType string so exchange/detection handlers keep behaving unchanged.
func reqKindToDetectType(kind string) string {
	switch kind {
	case "craft":
		return "craft"
	case "obtain":
		return "inventory"
	case "place":
		return "block_placed"
	case "machine":
		return "machine"
	case "side_configured":
		return "side_configured"
	default:
		return ""
	}
}
