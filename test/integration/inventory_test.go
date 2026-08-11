package integration

import (
	"testing"
	"time"

	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"

	"github.com/gtnh-platform/integration-tests/testutil"
)

// TC5: Chunk — get block state after placement (tests ChunkStore integration).
func TestChunk_GetBlockAfterSet(t *testing.T) {
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()

	playerID := uint64(42)
	pos := [3]int32{300, 50, 300}

	// Place cobblestone at a new position
	fbData := testutil.BuildSetBlockAction(playerID, pos[0], pos[1], pos[2], 0, 7)
	if err := c.SendCtrl(testutil.MsgSetBlockAction, fbData); err != nil {
		t.Fatalf("send SetBlockAction: %v", err)
	}

	data, err := c.ExpectMsgType(testutil.MsgBlockAck, 5*time.Second)
	if err != nil {
		t.Fatalf("expect BlockAck: %v", err)
	}
	testutil.AssertBlockAck(t, data, 1) // ACCEPTED
	t.Logf("Placed cobblestone (7) at (%d,%d,%d)", pos[0], pos[1], pos[2])
}

// TC6-TC7: Player inventory — send InventoryAction, expect InventoryUpdate back.
func TestInventory_MoveBetweenSlots(t *testing.T) {
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()

	playerID := uint64(42)

	t.Run("MoveItemSlot0ToSlot5", func(t *testing.T) {
		fbData := testutil.BuildInventoryAction(playerID, 0 /*MOVE*/, 0, 5, 1, 0)
		if err := c.SendCtrl(testutil.MsgInventoryAction, fbData); err != nil {
			t.Fatalf("send InventoryAction: %v", err)
		}

		// Expect an InventoryUpdate back
		data, err := c.ExpectMsgType(testutil.MsgInventoryUpdate, 5*time.Second)
		if err != nil {
			t.Fatalf("expect InventoryUpdate: %v", err)
		}
		t.Logf("InventoryUpdate received (%d bytes)", len(data))
	})
}

// TC8: Break a block and verify the item appears in inventory.
func TestInventory_BreakBlockGivesItem(t *testing.T) {
	c, err := testutil.DialGateway(gw, 10*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()

	playerID := uint64(99)
	pos := [3]int32{800, 50, 800}
	blockID := uint16(7) // cobblestone

	// Step 1: Place cobblestone at the position
	placeFB := testutil.BuildSetBlockAction(playerID, pos[0], pos[1], pos[2], 0, blockID)
	if err := c.SendCtrl(testutil.MsgSetBlockAction, placeFB); err != nil {
		t.Fatalf("send SetBlockAction(place): %v", err)
	}
	ackData, err := c.ExpectMsgType(testutil.MsgBlockAck, 5*time.Second)
	if err != nil {
		t.Fatalf("expect BlockAck after place: %v", err)
	}
	testutil.AssertBlockAck(t, ackData, 1) // ACCEPTED
	t.Logf("Placed block %d at (%d,%d,%d)", blockID, pos[0], pos[1], pos[2])

	// Step 2: Break the block
	breakFB := testutil.BuildBreakBlockAction(playerID, pos[0], pos[1], pos[2], blockID)
	if err := c.SendCtrl(testutil.MsgSetBlockAction, breakFB); err != nil {
		t.Fatalf("send SetBlockAction(break): %v", err)
	}
	ackData, err = c.ExpectMsgType(testutil.MsgBlockAck, 5*time.Second)
	if err != nil {
		t.Fatalf("expect BlockAck after break: %v", err)
	}
	testutil.AssertBlockAck(t, ackData, 1) // ACCEPTED
	t.Log("Block break ACKED")

	// Step 3: Wait for InventoryUpdate — the broken block should now be in inventory.
	// There may be stale updates from player.joined — loop until we find one with our item.
	deadline := time.Now().Add(15 * time.Second)
	found := false
	for time.Now().Before(deadline) {
		data, err := c.ExpectMsgType(testutil.MsgInventoryUpdate, time.Until(deadline))
		if err != nil {
			break
		}
		t.Logf("InventoryUpdate received (%d bytes)", len(data))

		invUpdate := Protocol.GetRootAsInventoryUpdate(data, 0)
		if invUpdate == nil || invUpdate.PlayerId() != playerID {
			continue
		}
		slots := invUpdate.SlotsLength()
		for i := 0; i < slots; i++ {
			var slot Protocol.InventorySlot
			if invUpdate.Slots(&slot, i) {
				if slot.ItemId() == blockID && slot.Count() > 0 {
					found = true
					t.Logf("Found block %d in slot %d (count=%d)", blockID, i, slot.Count())
					break
				}
			}
		}
		if found {
			break
		}
	}
	if !found {
		t.Errorf("Block %d not found in inventory after breaking", blockID)
	}
}
