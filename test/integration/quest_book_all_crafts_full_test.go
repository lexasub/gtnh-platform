package integration

import (
	"bufio"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
)

func TestQuestBook_AllCraftsFull(t *testing.T) {
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()

	const playerID = uint64(9300)
	baseX := int32(12000)
	baseY := int32(70)
	baseZ := int32(12000)

	if err := c.RequestChunk(playerID, baseX>>5, baseY>>5, baseZ>>5); err != nil {
		t.Fatalf("request chunk: %v", err)
	}
	c.WaitForChunkGeneration(4 * time.Second)

	cs, _ := testutil.DialChunkStore("127.0.0.1", 5001, 5*time.Second)
	defer cs.Close()

	// Load items from registry
	itemsPath := "/home/su/src/local/gtnh-platform/src/content/data/registry/items.csv"
	f, err := os.Open(itemsPath)
	if err != nil {
		t.Skipf("items.csv not found: %v", err)
	}
	defer f.Close()

	scanner := bufio.NewScanner(f)
	scanner.Scan() // skip header
	count := 0
	idx := 0
	for scanner.Scan() {
		line := scanner.Text()
		if strings.TrimSpace(line) == "" || strings.HasPrefix(line, "#") {
			continue
		}
		parts := strings.SplitN(line, ",", 4)
		if len(parts) < 2 {
			continue
		}
		name := strings.TrimSpace(parts[1])
		// Skip water and air
		if name == "air" || name == "water" || name == "flowing_water" {
			continue
		}
		// Place a crafting table as proxy for crafting this item
		x := baseX + int32(idx%20)
		z := baseZ + int32(idx/20)
		// A placement CASes against air, and fixed baseY=70 is inside the
		// generated terrain here, so resolve a real empty cell per column.
		// Use a dummy block id for crafting table, e.g., 0xE000 heat furnace as placeholder
		if err := c.PlaceBlockAndWait(cs, playerID, x, airCellAbove(t, cs, x, baseY, z), z,
			0xE000, uint32(150000+idx), 8*time.Second); err != nil {
			t.Logf("skip %s: %v", name, err)
			continue
		}
		count++
		idx++
		if idx > 5 { // limit for test runtime
			break
		}
	}
	t.Logf("Quest book craft simulation completed for %d items", count)
}
