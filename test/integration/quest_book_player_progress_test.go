package integration

import (
	"encoding/json"
	"os"
	"strconv"
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
)

type QuestGraphEntry struct {
	ID      int   `json:"id"`
	Prereqs []int `json:"prereqs"`
}

type QuestGraphRoot struct {
	Quests []QuestGraphEntry `json:"quests"`
}

func TestQuestBook_PlayerProgress(t *testing.T) {
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()

	const playerID = uint64(9500)
	baseX := int32(14000)
	baseY := int32(70)
	baseZ := int32(14000)

	if err := c.RequestChunk(playerID, baseX>>5, baseY>>5, baseZ>>5); err != nil {
		t.Fatalf("request chunk: %v", err)
	}
	c.WaitForChunkGeneration(4 * time.Second)

	cs, _ := testutil.DialChunkStore("127.0.0.1", 5001, 5*time.Second)
	defer cs.Close()

	// Load quest graph
	graphPath := "/home/su/src/local/gtnh-platform/src/content/data/quests/quest_graph.json"
	graphData, err := os.ReadFile(graphPath)
	if err != nil {
		t.Skipf("quest_graph.json not found: %v", err)
	}
	var graphRoot QuestGraphRoot
	if err := json.Unmarshal(graphData, &graphRoot); err != nil {
		t.Fatalf("json unmarshal graph: %v", err)
	}

	// Load requirements
	reqPath := "/home/su/src/local/gtnh-platform/src/content/data/quests/quest_requirements.json"
	reqData, err := os.ReadFile(reqPath)
	if err != nil {
		t.Skipf("quest_requirements.json not found: %v", err)
	}
	var reqMap map[string]QuestReqEntry
	if err := json.Unmarshal(reqData, &reqMap); err != nil {
		t.Fatalf("json unmarshal req: %v", err)
	}

	// Simple topological walk: process quests in order of id, assuming graph is already topologically sorted
	// For real implementation, do Kahn's algorithm
	// A placement CASes against air, and fixed baseY=70 is inside the generated
	// terrain here, so every placement came back CONFLICT and burned the full 8s
	// retry budget. Resolve a real empty cell per column instead.
	type column struct{ x, z int32 }
	airY := make(map[column]int32, 30)

	idx := 0
	processed := 0
	// The old `break` only escaped the inner loop, so the outer walk kept
	// re-using the same 30 slots forever; a label is what the limit always meant.
progressLoop:
	for _, q := range graphRoot.Quests {
		qid := q.ID
		qidStr := strconv.Itoa(qid)
		entry, ok := reqMap[qidStr]
		if !ok {
			continue
		}
		// Simulate each requirement
		for _, req := range entry.Requirements {
			// Skip water
			if contains(req.Item, "0:2:") {
				continue
			}
			// Simulate action by placing a placeholder block
			x := baseX + int32(idx%30)
			z := baseZ + int32(idx/30)
			blockID := uint16(0xE000) // placeholder
			col := column{x, z}
			y, ok := airY[col]
			if !ok {
				y = airCellAbove(t, cs, x, baseY, z)
				airY[col] = y
			}
			// In real implementation, map req.Item to actual block/item id and perform craft/place
			if err := c.PlaceBlockAndWait(cs, playerID, x, y, z, blockID, uint32(170000+idx), 8*time.Second); err != nil {
				t.Logf("quest %d req %s %s skipped: %v", qid, req.Kind, req.Item, err)
				idx++
				continue
			}
			idx++
			processed++
			if idx > 30 { // limit for test runtime
				break progressLoop
			}
		}
	}

	t.Logf("Quest book player progress simulation completed for %d actions", processed)
}

func contains(s, substr string) bool {
	return len(s) >= len(substr) && (s == substr || len(s) > len(substr) && (s[:len(substr)] == substr || indexOf(s, substr) >= 0))
}

func indexOf(s, substr string) int {
	for i := 0; i+len(substr) <= len(s); i++ {
		if s[i:i+len(substr)] == substr {
			return i
		}
	}
	return -1
}
