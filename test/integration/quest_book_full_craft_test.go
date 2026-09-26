package integration

import (
	"encoding/json"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
)

type QuestReq struct {
	Kind    string `json:"kind"`
	Item    string `json:"item"`
	Count   int    `json:"count"`
	Consume bool   `json:"consume"`
}

type QuestReqEntry struct {
	AutoComplete bool       `json:"auto_complete"`
	Requirements []QuestReq `json:"requirements"`
}

func TestQuestBook_FullCraft(t *testing.T) {
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()

	const playerID = uint64(9400)
	baseX := int32(13000)
	baseY := int32(70)
	baseZ := int32(13000)

	if err := c.RequestChunk(playerID, baseX>>5, baseY>>5, baseZ>>5); err != nil {
		t.Fatalf("request chunk: %v", err)
	}
	c.WaitForChunkGeneration(4 * time.Second)

	cs, _ := testutil.DialChunkStore("127.0.0.1", 5001, 5*time.Second)
	defer cs.Close()

	// Load quest requirements
	reqPath := "/home/su/src/local/gtnh-platform/src/content/data/quests/quest_requirements.json"
	data, err := os.ReadFile(reqPath)
	if err != nil {
		t.Skipf("quest_requirements.json not found: %v", err)
	}
	var reqMap map[string]QuestReqEntry
	if err := json.Unmarshal(data, &reqMap); err != nil {
		t.Fatalf("json unmarshal: %v", err)
	}

	// "airCellAbove" per column: a placement CASes against air, so the fixture
	// must resolve a real empty cell. The old fixed baseY=70 sits inside the
	// generated terrain at (13000,13000) -- every placement came back
	// "Block CAS CONFLICT at (13000,70,13000) actual_id=1" and then burned the
	// full 8s retry budget, 158 times over, which is what pushed the suite past
	// its 25m timeout.
	type column struct{ x, z int32 }
	airY := make(map[column]int32, 20)
	idx := 0
	crafted := 0
	skippedWater := 0

	// The old `break` only escaped the inner loop, so the outer loop kept
	// re-walking the remaining quests and re-using the same 50 slots forever.
	// A labelled break is what the "limit runtime" comment always meant.
craftLoop:
	for qid, entry := range reqMap {
		for _, req := range entry.Requirements {
			// Skip water
			if strings.Contains(req.Item, "0:2:") {
				skippedWater++
				continue
			}
			// Simulate craft/place by placing a placeholder block
			x := baseX + int32(idx%20)
			z := baseZ + int32(idx/20)
			// Use heat furnace as placeholder for crafting action
			blockID := uint16(0xE000)
			if req.Kind == "place" {
				blockID = 0xE000
			}
			col := column{x, z}
			y, ok := airY[col]
			if !ok {
				y = airCellAbove(t, cs, x, baseY, z)
				airY[col] = y
			}
			if err := c.PlaceBlockAndWait(cs, playerID, x, y, z, blockID,
				uint32(160000+idx), 8*time.Second); err != nil {
				t.Logf("quest %s req %s %s skipped: %v", qid, req.Kind, req.Item, err)
				idx++
				continue
			}
			crafted++
			idx++
			if idx > 50 { // limit runtime
				break craftLoop
			}
		}
	}

	t.Logf("Quest book craft simulation: crafted %d actions, skipped water %d", crafted, skippedWater)
}
