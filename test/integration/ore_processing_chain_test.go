package integration

import (
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
)

func TestOreProcessingChain_MaceratorFurnaceCompressor(t *testing.T) {
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()

	const playerID = uint64(9002)
	cornerX := int32(8000)
	cornerY := int32(70)
	cornerZ := int32(8000)

	if err := c.RequestChunk(playerID, cornerX>>5, cornerY>>5, cornerZ>>5); err != nil {
		t.Fatalf("request chunk: %v", err)
	}
	c.WaitForChunkGeneration(4 * time.Second)

	cs, err := testutil.DialChunkStore("127.0.0.1", 5001, 5*time.Second)
	if err != nil {
		t.Fatalf("dial ChunkStore: %v", err)
	}
	defer cs.Close()

	// A placement CASes against air, so each column needs a verified empty
	// cell. The old fixture pinned y=70, which is inside the generated terrain
	// at (8000,8000) — every placement came back CONFLICT with actual_id=1.
	// The row is placed on the first air cell at or above cornerY per column.
	//
	// Block ids are the packed machines.yaml values; the old comment claimed
	// 0xE004 for the compressor but the code placed 0xE201, which is the real
	// steam_compressor ("1110:001:1").
	placements := []struct {
		dx   int32
		id   uint16
		what string
	}{
		{0, 0xE001, "heat macerator"},     // "1110:000:1"
		{2, 0xE000, "heat furnace"},       // "1110:000:0"
		{4, 0xE201, "steam compressor"},   // "1110:001:1"
		{1, 0xF800, "item pipe"},          // "1111:10:0"
		{3, 0xF800, "item pipe"},          // "1111:10:0"
		{6, 0xE800, "creative generator"}, // "1110:100:0"
		{5, 0xF400, "tin cable"},          // "1111:01:0"
	}

	for i, p := range placements {
		x := cornerX + p.dx
		y := airCellAbove(t, cs, x, cornerY, cornerZ)
		if err := c.PlaceBlockAndWait(cs, playerID, x, y, cornerZ, p.id,
			uint32(60000+i), 8*time.Second); err != nil {
			t.Fatalf("place %s 0x%04X at (%d,%d,%d): %v", p.what, p.id, x, y, cornerZ, err)
		}
		t.Logf("placed %s 0x%04X at (%d,%d,%d)", p.what, p.id, x, y, cornerZ)
	}
}
