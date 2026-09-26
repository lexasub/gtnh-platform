package integration

import (
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
)

func connect(t *testing.T) *testutil.GatewayClient {
	t.Helper()
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	return c
}

// blockReader is the authoritative block-state source airCellAbove needs.
// Both testutil.ChunkStoreClient and testutil.RouterClient speak the ChunkStore
// GetBlock RPC; this keeps the helper usable from either connection.
type blockReader interface {
	GetBlock(x, y, z int32, timeout time.Duration) (uint16, uint8, uint32, error)
}

// airCellAbove finds the lowest air cell at column (x,z) at or above yMin.
//
// A right-click placement does not CAS against the client's expected_block_id:
// ActionContext (src/game/actions/ActionContext.cpp:44-49) resolves the
// effective cell through faceAdjacentBlock() and then forces
// eff_expected = 0, i.e. "place against air". So a placement only succeeds
// when the effective cell is genuinely empty. Pinning a fixed y in a fixture
// makes the test depend on the terrain height the world generator happens to
// produce; querying ChunkStore for an air cell keeps it deterministic.
func airCellAbove(t *testing.T, cs blockReader, x, yMin, z int32) int32 {
	t.Helper()
	for y := yMin; y < 250; y++ {
		id, _, _, err := cs.GetBlock(x, y, z, 5*time.Second)
		if err != nil {
			t.Fatalf("ChunkStore GetBlock (%d,%d,%d): %v", x, y, z, err)
		}
		if id == 0 {
			return y
		}
	}
	t.Fatalf("no air cell in column (%d,%d) from y=%d upward", x, z, yMin)
	return 0
}

// placeAtCell sends the production placement frame for the block that occupies
// block cell (x,y,z).
//
// The wire protocol is off by one on purpose: the client reports the block it
// CLICKED and the face it hit, and the server derives the placement cell. With
// Face 0 (DOWN — the top face of the block below) the server decrements y, so
// the frame must carry y+1 to land on y. testutil.PlaceBlockAndWait uses the
// same convention (testutil/chunkstore.go:146-149).
func placeAtCell(playerID uint64, x, y, z int32, blockID uint16, reqID uint32) []byte {
	return testutil.BuildPlaceBlockActionWithOptions(playerID, x, y+1, z, 0, blockID,
		testutil.SetBlockActionOptions{RequestID: reqID, Face: 0})
}

// TestPlaceAtCellSendsAClaimableFrame is the Go-side guard for the defect
// gp-j23p recorded: TestChunk_GetBlockAfterSet used to send an EMPTY-HAND
// frame, which no handler claims, so the server correctly answered REJECTED
// and the test failed against correct behaviour.
//
// The reason that bug is easy to reintroduce is that the claim rule lives only
// in C++ — PlaceBlockHandler::isPlacementShape requires held_item != 0, not a
// mining tool and not a wrench (PlaceBlockHandler.cpp:32-36). A Go fixture
// that regresses to the no-options builder produces a byte-identical frame to a
// legitimate negative-path one, and the only symptom is a REJECTED that looks
// like a server bug five seconds later.
//
// This asserts the frame every placement fixture depends on. It is
// deterministic, needs no cluster, and fails at build time of the test rather
// than at 5 s into a placement.
func TestPlaceAtCellSendsAClaimableFrame(t *testing.T) {
	var pos Protocol.Vec3i
	action := Protocol.GetRootAsSetBlockAction(
		placeAtCell(42, 100, 60, 200, cobblestoneID, 41001), 0)

	// 1. RIGHT_MOUSE_CLICK — a LEFT_MOUSE_CLICK is a break, which
	// BreakBlockHandler claims instead (BreakBlockHandler.cpp:53-55).
	if action.Action() != Protocol.PlayerActionTypeRIGHT_MOUSE_CLICK {
		t.Errorf("action = %v, want RIGHT_MOUSE_CLICK", action.Action())
	}
	// 2. held_item != 0 — the empty hand is what gp-j23p was about.
	if action.HeldItem() == 0 {
		t.Error("held_item = 0: no handler claims an empty-hand right-click, " +
			"so the server answers REJECTED and the placement never happens")
	}
	// 3. held_item is the block being placed, matching NetClient::SendBlockAction
	// (NetClient.cpp:695-706), which writes the equipped item to both fields.
	if action.HeldItem() != cobblestoneID {
		t.Errorf("held_item = %d, want the placed block %d", action.HeldItem(), cobblestoneID)
	}
	// 4. Face 0 (DOWN) is what makes the server's --y land on the intended cell
	// rather than one below it (ActionContext faceAdjacentBlock).
	if action.Face() != 0 {
		t.Errorf("face = %d, want 0 (DOWN)", action.Face())
	}
	// 5. The coordinate is the CLICKED cell, so it is y+1 — placeAtCell adds
	// it, and the server subtracts it. Assert the pair, not the y.
	if action.Pos(&pos) == nil {
		t.Fatal("SetBlockAction has no pos struct")
	}
	if pos.X() != 100 || pos.Y() != 61 || pos.Z() != 200 {
		t.Errorf("clicked pos = (%d,%d,%d), want (100,61,200) — placeAtCell(…, y=60) "+
			"must send the cell ABOVE the target", pos.X(), pos.Y(), pos.Z())
	}
	// 6. Request correlation, so WaitForBlockAck can match the ACK and the
	// test does not consume a neighbouring test's.
	if action.RequestId() != 41001 {
		t.Errorf("request_id = %d, want 41001", action.RequestId())
	}
}

// primeChunk generates the chunk containing (x,y,z) before a CAS placement
// touches it. Gateway's automatic spawn request only covers the player's saved
// position (src/apps/gateway/gateway.cpp:116-131), so a placement far from
// spawn can otherwise lose the world-gen race and come back CONFLICT.
func primeChunk(t *testing.T, c *testutil.GatewayClient, x, y, z int32) {
	t.Helper()
	if err := c.RequestChunk(42, x>>5, y>>5, z>>5); err != nil {
		t.Fatalf("request chunk for (%d,%d,%d): %v", x, y, z, err)
	}
	c.WaitForChunkGeneration(4 * time.Second)
}

func dialChunkStore(t *testing.T) *testutil.ChunkStoreClient {
	t.Helper()
	cs, err := testutil.DialChunkStore("127.0.0.1", 5001, 5*time.Second)
	if err != nil {
		t.Fatalf("dial ChunkStore: %v", err)
	}
	return cs
}

func TestSetBlock_CAS_Accept(t *testing.T) {
	c := connect(t)
	defer c.Close()
	primeChunk(t, c, 100, 50, 200)

	cs := dialChunkStore(t)
	defer cs.Close()
	y := airCellAbove(t, cs, 100, 50, 200)

	const reqID = uint32(41001)
	if err := c.SendCtrl(testutil.MsgSetBlockAction,
		placeAtCell(42, 100, y, 200, 2, reqID)); err != nil {
		t.Fatalf("send SetBlockAction: %v", err)
	}
	// The placement protocol's immediate client acknowledgement is ACCEPTED
	// (CasRunner.cpp:15-17); the authoritative result is the ChunkStore commit.
	data, err := c.WaitForBlockAck(reqID, Protocol.BlockAckStatusACCEPTED, 5*time.Second)
	if err != nil {
		t.Fatalf("expect ACCEPTED: %v", err)
	}
	testutil.AssertBlockAck(t, data, Protocol.BlockAckStatusACCEPTED)

	if err := cs.WaitForBlock(100, y, 200, 2, 5*time.Second); err != nil {
		t.Fatalf("ChunkStore never committed block 2 at (100,%d,200): %v", y, err)
	}
}

func TestSetBlock_CAS_Conflict(t *testing.T) {
	const (
		x = int32(110)
		z = int32(210)
	)

	c := connect(t)
	defer c.Close()
	primeChunk(t, c, x, 50, z)

	cs := dialChunkStore(t)
	defer cs.Close()
	y := airCellAbove(t, cs, x, 50, z)

	// First placement fills the air cell, so the cell is no longer air.
	const firstReqID = uint32(41010)
	if err := c.SendCtrl(testutil.MsgSetBlockAction,
		placeAtCell(42, x, y, z, 1, firstReqID)); err != nil {
		t.Fatalf("send first SetBlockAction: %v", err)
	}
	if _, err := c.WaitForBlockAck(firstReqID, Protocol.BlockAckStatusACCEPTED, 5*time.Second); err != nil {
		t.Fatalf("first placement: %v", err)
	}
	if err := cs.WaitForBlock(x, y, z, 1, 5*time.Second); err != nil {
		t.Fatalf("first placement never committed: %v", err)
	}

	// A placement into the now-occupied cell must not win: the effective
	// expected id is air, the cell holds block 1, so CAS fails.
	const secondReqID = uint32(41011)
	if err := c.SendCtrl(testutil.MsgSetBlockAction,
		placeAtCell(42, x, y, z, 2, secondReqID)); err != nil {
		t.Fatalf("send second SetBlockAction: %v", err)
	}
	// SimCore sends optimistic ACCEPTED first, then CONFLICT after the
	// ChunkStore CAS completes (CasRunner.cpp:15-17 then :44-46).
	if _, err := c.WaitForBlockAck(secondReqID, Protocol.BlockAckStatusCONFLICT, 5*time.Second); err != nil {
		t.Fatalf("expected CONFLICT when placing into an occupied cell: %v", err)
	}
}

func TestSetBlock_CAS_AcceptAfterCorrectExpected(t *testing.T) {
	const (
		x = int32(120)
		z = int32(220)
	)

	c := connect(t)
	defer c.Close()
	primeChunk(t, c, x, 50, z)

	cs := dialChunkStore(t)
	defer cs.Close()
	y := airCellAbove(t, cs, x, 50, z)

	const firstReqID = uint32(41020)
	if err := c.SendCtrl(testutil.MsgSetBlockAction,
		placeAtCell(42, x, y, z, 5, firstReqID)); err != nil {
		t.Fatalf("send first SetBlockAction: %v", err)
	}
	if _, err := c.WaitForBlockAck(firstReqID, Protocol.BlockAckStatusACCEPTED, 5*time.Second); err != nil {
		t.Fatalf("first placement: %v", err)
	}
	if err := cs.WaitForBlock(x, y, z, 5, 5*time.Second); err != nil {
		t.Fatalf("first placement never committed: %v", err)
	}

	// Break the block again so the cell is air, then replace it. This proves a
	// fresh placement into the same column is accepted once the expected
	// condition holds again.
	if err := c.SendCtrl(testutil.MsgSetBlockAction,
		testutil.BuildBreakBlockAction(42, x, y, z, 5)); err != nil {
		t.Fatalf("send break: %v", err)
	}
	if err := cs.WaitForBlock(x, y, z, 0, 5*time.Second); err != nil {
		t.Fatalf("break never committed: %v", err)
	}

	const secondReqID = uint32(41021)
	if err := c.SendCtrl(testutil.MsgSetBlockAction,
		placeAtCell(42, x, y, z, 7, secondReqID)); err != nil {
		t.Fatalf("send second SetBlockAction: %v", err)
	}
	if _, err := c.WaitForBlockAck(secondReqID, Protocol.BlockAckStatusACCEPTED, 5*time.Second); err != nil {
		t.Fatalf("expected ACCEPTED once the cell is air again: %v", err)
	}
	if err := cs.WaitForBlock(x, y, z, 7, 5*time.Second); err != nil {
		t.Fatalf("ChunkStore never committed block 7 at (%d,%d,%d): %v", x, y, z, err)
	}
}
