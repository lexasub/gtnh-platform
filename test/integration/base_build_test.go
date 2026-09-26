package integration

import (
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
)

func TestBase_BuildMultiMachine(t *testing.T) {
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()

	const playerID = uint64(9001)
	// origin far from existing tests
	cornerX := int32(7000)
	cornerY := int32(70)
	cornerZ := int32(7000)

	if err := c.RequestChunk(playerID, cornerX>>5, cornerY>>5, cornerZ>>5); err != nil {
		t.Fatalf("request chunk: %v", err)
	}
	c.WaitForChunkGeneration(4 * time.Second)

	// place a simple 3x3 casing frame as base foundation
	placements := []struct {
		x, y, z int32
		id      uint16
	}{
		{cornerX, cornerY, cornerZ, 0xEE05},
		{cornerX + 1, cornerY, cornerZ, 0xEE05},
		{cornerX + 2, cornerY, cornerZ, 0xEE05},
		{cornerX, cornerY, cornerZ + 1, 0xEE05},
		{cornerX + 1, cornerY, cornerZ + 1, 0xEE05},
		{cornerX + 2, cornerY, cornerZ + 1, 0xEE05},
		{cornerX, cornerY, cornerZ + 2, 0xEE05},
		{cornerX + 1, cornerY, cornerZ + 2, 0xEE05},
		{cornerX + 2, cornerY, cornerZ + 2, 0xEE05},
		// creative EU source
		{cornerX + 4, cornerY + 3, cornerZ + 1, 0xE800},
		{cornerX + 3, cornerY + 3, cornerZ + 1, 0xF400},
	}

	cs, _ := testutil.DialChunkStore("127.0.0.1", 5001, 5*time.Second)
	defer cs.Close()

	for i, p := range placements {
		if err := c.PlaceBlockAndWait(cs, playerID, p.x, p.y, p.z, p.id, uint32(50000+i), 8*time.Second); err != nil {
			t.Fatalf("place block %x at (%d,%d,%d): %v", p.id, p.x, p.y, p.z, err)
		}
	}
	t.Logf("Base foundation placed at %d,%d,%d", cornerX, cornerY, cornerZ)
}
