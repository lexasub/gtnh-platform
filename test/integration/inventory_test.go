package integration

import (
	"testing"
	"time"

	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"

	"github.com/gtnh-platform/integration-tests/testutil"
)

// cobblestoneID is "0:0:2" packed (src/content/data/registry/items.csv:9,
// src/engine/registry/ItemId.h:85-128). The old literal 7 does not exist in
// the current registry — it was a pre-packed-ID assumption.
const cobblestoneID uint16 = 2

// TC5: Chunk — get block state after placement (tests ChunkStore integration).
func TestChunk_GetBlockAfterSet(t *testing.T) {
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()

	playerID := uint64(42)
	const (
		x = int32(300)
		y = int32(50)
		z = int32(300)
	)

	// Gateway's automatic CHUNK_REQUEST only covers the player's saved spawn
	// position, so a placement this far away must generate the chunk first or
	// the CAS loses the world-gen race and comes back CONFLICT.
	if err := c.RequestChunk(playerID, x>>5, y>>5, z>>5); err != nil {
		t.Fatalf("request chunk: %v", err)
	}
	c.WaitForChunkGeneration(4 * time.Second)

	cs, err := testutil.DialChunkStore("127.0.0.1", 5001, 5*time.Second)
	if err != nil {
		t.Fatalf("dial ChunkStore: %v", err)
	}
	defer cs.Close()
	// Place on a verified air cell: a placement CASes against air, and the
	// terrain height at (300,50,300) is generator output, not a constant.
	placeY := airCellAbove(t, cs, x, y, z)

	const reqID = uint32(42001)
	if err := c.SendCtrl(testutil.MsgSetBlockAction,
		placeAtCell(playerID, x, placeY, z, cobblestoneID, reqID)); err != nil {
		t.Fatalf("send SetBlockAction: %v", err)
	}
	data, err := c.WaitForBlockAck(reqID, Protocol.BlockAckStatusACCEPTED, 5*time.Second)
	if err != nil {
		t.Fatalf("expect BlockAck: %v", err)
	}
	testutil.AssertBlockAck(t, data, Protocol.BlockAckStatusACCEPTED)

	// The ACK is optimistic; the block is placed when ChunkStore commits.
	if err := cs.WaitForBlock(x, placeY, z, cobblestoneID, 5*time.Second); err != nil {
		t.Fatalf("ChunkStore never committed cobblestone at (%d,%d,%d): %v", x, placeY, z, err)
	}
	t.Logf("Placed cobblestone (%d) at (%d,%d,%d)", cobblestoneID, x, placeY, z)
}

// TC6-TC7: Player inventory — send InventoryAction, expect InventoryUpdate back.
func TestInventory_MoveBetweenSlots(t *testing.T) {
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()

	// Gateway forwards queued player.inventory.update pushes without
	// correlation, so the player's whole inventory is the predicate — not the
	// first frame of that type. Correlate by player identity (gp-c56).
	const playerID = uint64(42)

	// Give the player a known stack to move. ITEM_ACTION is the production
	// creative-menu grant path
	// (src/game/actions/PlayerActionDispatcher.cpp:20-29).
	if err := c.SendCtrl(testutil.MsgPlayerAction, testutil.BuildPlayerAction(
		playerID, Protocol.PlayerActionTypeITEM_ACTION, 0, 0, 0, cobblestoneID, 4)); err != nil {
		t.Fatalf("send ITEM_ACTION grant: %v", err)
	}
	granted, err := c.WaitForInventoryItem(playerID, cobblestoneID, 4, 5*time.Second)
	if err != nil {
		t.Fatalf("wait for the granted stack: %v", err)
	}

	// The grant lands in the first free slot, which is not necessarily slot 0.
	// Click whatever slot actually holds it, so the click is a real move
	// instead of a no-op on an empty cell (ApplyClick returns false for an
	// empty target and the server publishes nothing).
	srcSlot := testutil.FirstSlotWithItem(granted, cobblestoneID)
	if srcSlot < 0 {
		t.Fatalf("no slot holds item %d in the granted snapshot", cobblestoneID)
	}

	// Grab the stack: left-click the source slot so it moves to the
	// server-owned cursor (InventoryClick.h:148-153). Assert the SERVER moved
	// it — the stack leaves the player grid and appears as the cursor — rather
	// than "an update arrived" (gp-c56).
	if err := c.SendCtrl(testutil.MsgInventoryAction,
		testutil.BuildInventoryActionWithOptions(playerID,
			uint8(0 /*kActionClick*/), 0 /*left button*/, 0, 0, /*player inventory*/
			uint16(srcSlot), 0)); err != nil {
		t.Fatalf("send pick-up click: %v", err)
	}

	deadline := time.Now().Add(5 * time.Second)
	moved := false
	for time.Now().Before(deadline) {
		msgType, data, err := c.ReadCtrl(time.Until(deadline))
		if err != nil {
			t.Fatalf("read post-click snapshot: %v", err)
		}
		if msgType != testutil.MsgInventoryUpdate || len(data) == 0 {
			continue
		}
		update := Protocol.GetRootAsInventoryUpdate(data, 0)
		if update.PlayerId() != playerID {
			continue
		}
		if testutil.InventoryItemCount(data, cobblestoneID) == 0 {
			moved = true
			break
		}
	}
	if !moved {
		t.Fatalf("pick-up left item %d in the player grid (source slot %d)", cobblestoneID, srcSlot)
	}
}

// TC8: Break a block and verify the item appears in inventory.
func TestInventory_BreakBlockGivesItem(t *testing.T) {
	c, err := testutil.DialGateway(gw, 10*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()

	playerID := uint64(99)
	const (
		x = int32(800)
		y = int32(50)
		z = int32(800)
	)

	if err := c.RequestChunk(playerID, x>>5, y>>5, z>>5); err != nil {
		t.Fatalf("request chunk: %v", err)
	}
	c.WaitForChunkGeneration(4 * time.Second)

	cs, err := testutil.DialChunkStore("127.0.0.1", 5001, 5*time.Second)
	if err != nil {
		t.Fatalf("dial ChunkStore: %v", err)
	}
	defer cs.Close()
	placeY := airCellAbove(t, cs, x, y, z)

	// Step 1: Place cobblestone at the position.
	const placeReqID = uint32(43001)
	if err := c.SendCtrl(testutil.MsgSetBlockAction,
		placeAtCell(playerID, x, placeY, z, cobblestoneID, placeReqID)); err != nil {
		t.Fatalf("send SetBlockAction(place): %v", err)
	}
	if _, err := c.WaitForBlockAck(placeReqID, Protocol.BlockAckStatusACCEPTED, 5*time.Second); err != nil {
		t.Fatalf("expect BlockAck after place: %v", err)
	}
	if err := cs.WaitForBlock(x, placeY, z, cobblestoneID, 5*time.Second); err != nil {
		t.Fatalf("place never committed: %v", err)
	}
	t.Logf("Placed block %d at (%d,%d,%d)", cobblestoneID, x, placeY, z)

	// Step 2: Break the block.
	if err := c.SendCtrl(testutil.MsgSetBlockAction,
		testutil.BuildBreakBlockAction(playerID, x, placeY, z, cobblestoneID)); err != nil {
		t.Fatalf("send SetBlockAction(break): %v", err)
	}
	if err := cs.WaitForBlock(x, placeY, z, 0, 5*time.Second); err != nil {
		t.Fatalf("break never committed: %v", err)
	}
	t.Log("Block break committed")

	// Step 3: Wait for the InventoryUpdate that proves the drop landed.
	// Gateway forwards queued player.inventory.update pushes without
	// correlation and the server splits stacks across slots, so correlate by
	// predicate instead of taking the first frame (gp-c56).
	if _, err := c.WaitForInventoryItem(playerID, cobblestoneID, 1, 15*time.Second); err != nil {
		t.Errorf("block %d not in inventory after breaking: %v", cobblestoneID, err)
	}
}
