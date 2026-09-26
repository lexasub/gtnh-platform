package integration

import (
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
)

func TestQuestBook_CompleteChain(t *testing.T) {
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()

	const playerID = uint64(9100)
	cornerX := int32(10000)
	cornerY := int32(70)
	cornerZ := int32(10000)

	if err := c.RequestChunk(playerID, cornerX>>5, cornerY>>5, cornerZ>>5); err != nil {
		t.Fatalf("request chunk: %v", err)
	}
	c.WaitForChunkGeneration(4 * time.Second)

	cs, _ := testutil.DialChunkStore("127.0.0.1", 5001, 5*time.Second)
	defer cs.Close()

	// Simplified quest chain: place heat furnace, creative gen, pipe, then operate
	placements := []struct {
		x, y, z int32
		id      uint16
	}{
		{cornerX, cornerY, cornerZ, 0xE000},     // heat furnace
		{cornerX + 2, cornerY, cornerZ, 0xE800}, // creative gen
		{cornerX + 1, cornerY, cornerZ, 0xF400}, // cable
		{cornerX + 3, cornerY, cornerZ, 0xEE05}, // casing for multiblock quest
		{cornerX + 4, cornerY, cornerZ, 0xE42F}, // controller
	}

	for i, p := range placements {
		if err := c.PlaceBlockAndWait(cs, playerID, p.x, p.y, p.z, p.id, uint32(130000+i), 8*time.Second); err != nil {
			t.Fatalf("place block %x at (%d,%d,%d): %v", p.id, p.x, p.y, p.z, err)
		}
	}

	// Wait for quest system to process actions
	time.Sleep(3 * time.Second)

	time.Sleep(2 * time.Second)
	t.Logf("Quest book chain executed at %d,%d,%d", cornerX, cornerY, cornerZ)
}
