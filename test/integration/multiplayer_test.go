package integration

import (
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
)

// clientPair is one connected client: its ctrl socket and its bulk socket.
// Both are needed for every multiplayer assertion — ctrl carries the welcome,
// the ACKs and the requests, bulk carries chunk data and block updates.
type clientPair struct {
	ctrl      *testutil.GatewayClient
	bulk      *testutil.GatewayClient
	sessionID uint64
	playerID  uint64
}

func (p *clientPair) Close() {
	p.ctrl.Close()
	p.bulk.Close()
}

// dialClientPair performs the production handshake: ctrl first with the
// welcome, then a bulk socket paired to that session. Anything less is not a
// connected client — the gateway serves an unpaired bulk socket to nobody and
// drops frames whose player_id is not the one it assigned.
func dialClientPair(t *testing.T, addr testutil.GatewayAddress) *clientPair {
	t.Helper()
	ctrl, err := testutil.DialGatewaySession(addr, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	bulk, err := testutil.DialGatewayBulk(addr, ctrl.SessionID(), 5*time.Second)
	if err != nil {
		ctrl.Close()
		t.Fatalf("Gateway bulk port not reachable: %v", err)
	}
	return &clientPair{
		ctrl: ctrl, bulk: bulk,
		sessionID: ctrl.SessionID(), playerID: ctrl.PlayerID(),
	}
}

// expectChunkAt waits for a CompressedChunk frame for exactly (cx,cy,cz).
// Frames for other chunks are consumed: a session legitimately receives
// several chunks, and the assertion is about this one coordinate.
func expectChunkAt(t *testing.T, c *testutil.GatewayClient, cx, cy, cz int32, timeout time.Duration) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for {
		remaining := time.Until(deadline)
		if remaining <= 0 {
			t.Fatalf("no CompressedChunk for chunk (%d,%d,%d) within %s", cx, cy, cz, timeout)
		}
		msgType, data, err := c.ReadCtrl(remaining)
		if err != nil {
			t.Fatalf("waiting for chunk (%d,%d,%d): %v", cx, cy, cz, err)
		}
		if msgType != testutil.MsgCompressedChunk {
			continue
		}
		chunk := Protocol.GetRootAsCompressedChunkData(data, 0)
		if chunk == nil || chunk.Coord(nil) == nil {
			continue
		}
		coord := chunk.Coord(nil)
		if coord.X() == cx && coord.Y() == cy && coord.Z() == cz {
			return
		}
	}
}

// TestGateway_SessionsAreIndependent proves two clients coexist and that each
// one is told a different identity. A singleton gateway cannot pass this: it
// has one player_id, and the second connect silently evicts the first client.
func TestGateway_SessionsAreIndependent(t *testing.T) {
	first := dialClientPair(t, gw)
	defer first.Close()
	second := dialClientPair(t, gw)
	defer second.Close()

	if first.playerID == second.playerID {
		t.Fatalf("both clients were assigned player %d — the gateway has one shared identity",
			first.playerID)
	}
	if first.sessionID == second.sessionID {
		t.Fatalf("both clients were assigned session %d", first.sessionID)
	}
	if first.playerID == 0 || second.playerID == 0 {
		t.Fatalf("server assigned a zero identity (first=%d second=%d)",
			first.playerID, second.playerID)
	}

	// The first client is still alive after the second one connected: it can
	// still be served and can still act under its own identity.
	if err := first.ctrl.RequestChunk(first.playerID, 12, 1, 12); err != nil {
		t.Fatalf("first client cannot send after the second connected: %v", err)
	}
	expectChunkAt(t, first.bulk, 12, 1, 12, 15*time.Second)
	t.Logf("client A: session %d player %d; client B: session %d player %d — both live",
		first.sessionID, first.playerID, second.sessionID, second.playerID)
}

// TestGateway_BlockChangeReachesOtherClient is the multiplayer requirement in
// one test: a block placed by one client is observable on the other client's
// bulk stream, while the actor still receives its own BlockAck.
func TestGateway_BlockChangeReachesOtherClient(t *testing.T) {
	const (
		x = int32(500)
		z = int32(500)
	)

	actor := dialClientPair(t, gw)
	defer actor.Close()
	observer := dialClientPair(t, gw)
	defer observer.Close()

	if err := actor.ctrl.RequestChunk(actor.playerID, x>>5, 50>>5, z>>5); err != nil {
		t.Fatalf("actor chunk request failed: %v", err)
	}
	actor.ctrl.WaitForChunkGeneration(4 * time.Second)

	cs, err := testutil.DialChunkStore("127.0.0.1", 5001, 5*time.Second)
	if err != nil {
		t.Fatalf("dial ChunkStore: %v", err)
	}
	defer cs.Close()
	y := airCellAbove(t, cs, x, 50, z)

	const placeReqID = uint32(52001)
	if err := actor.ctrl.SendCtrl(testutil.MsgSetBlockAction,
		placeAtCell(actor.playerID, x, y, z, stoneID, placeReqID)); err != nil {
		t.Fatalf("actor SetBlockAction: %v", err)
	}

	if _, err := actor.ctrl.WaitForBlockAck(placeReqID, Protocol.BlockAckStatusACCEPTED, 5*time.Second); err != nil {
		t.Fatalf("actor never got BlockAck: %v", err)
	}

	deadline := time.Now().Add(5 * time.Second)
	for {
		remaining := time.Until(deadline)
		if remaining <= 0 {
			t.Fatalf("observer never saw the block change at (%d,%d,%d)", x, y, z)
		}
		msgType, data, err := observer.bulk.ReadCtrl(remaining)
		if err != nil {
			t.Fatalf("observer waiting for block update: %v", err)
		}
		if msgType != testutil.MsgBlockUpdate {
			continue
		}
		ev := Protocol.GetRootAsBlockChangedEvent(data, 0)
		if ev == nil || ev.Pos(nil) == nil {
			continue
		}
		if ev.Pos(nil).X() == x && ev.Pos(nil).Y() == y && ev.Pos(nil).Z() == z {
			if ev.BlockId() != stoneID {
				t.Fatalf("observer got block_id %d at (%d,%d,%d), want %d",
					ev.BlockId(), x, y, z, stoneID)
			}
			return
		}
	}
}

// TestGateway_ChunkStreamIsPerSession proves chunk delivery follows the
// request: a chunk requested for one session is not pushed to another session
// that never asked for it.
//
// Against a singleton Gateway the chunk goes to whichever connection was
// accepted last, so the observer sees a chunk it never requested.
func TestGateway_ChunkStreamIsPerSession(t *testing.T) {
	const (
		cx = int32(37)
		cy = int32(1)
		cz = int32(37)
	)

	requester := dialClientPair(t, gw)
	defer requester.Close()
	observer := dialClientPair(t, gw)
	defer observer.Close()

	if err := requester.ctrl.RequestChunk(requester.playerID, cx, cy, cz); err != nil {
		t.Fatalf("requester chunk request failed: %v", err)
	}

	// The requester must receive what it asked for.
	expectChunkAt(t, requester.bulk, cx, cy, cz, 15*time.Second)

	// The observer must not. Frames for its own spawn chunk are ignored.
	deadline := time.Now().Add(4 * time.Second)
	for {
		remaining := time.Until(deadline)
		if remaining <= 0 {
			return
		}
		msgType, data, err := observer.bulk.ReadCtrl(remaining)
		if err != nil {
			return // read timeout: nothing arrived, which is the expected outcome
		}
		if msgType != testutil.MsgCompressedChunk {
			continue
		}
		chunk := Protocol.GetRootAsCompressedChunkData(data, 0)
		if chunk == nil || chunk.Coord(nil) == nil {
			continue
		}
		coord := chunk.Coord(nil)
		if coord.X() == cx && coord.Y() == cy && coord.Z() == cz {
			t.Fatalf("observer received chunk (%d,%d,%d) it never requested — "+
				"chunk delivery is not per-session", cx, cy, cz)
		}
	}
}

// TestGateway_BlockChangeIsNotEchoedToSource is the other half of peer
// visibility: the actor already applied its own change optimistically and got
// the authoritative BlockAck, so relaying the world event back to it would
// double-apply the change.
func TestGateway_BlockChangeIsNotEchoedToSource(t *testing.T) {
	const (
		x = int32(520)
		z = int32(520)
	)

	actor := dialClientPair(t, gw)
	defer actor.Close()

	if err := actor.ctrl.RequestChunk(actor.playerID, x>>5, 50>>5, z>>5); err != nil {
		t.Fatalf("actor chunk request failed: %v", err)
	}
	actor.ctrl.WaitForChunkGeneration(4 * time.Second)

	cs, err := testutil.DialChunkStore("127.0.0.1", 5001, 5*time.Second)
	if err != nil {
		t.Fatalf("dial ChunkStore: %v", err)
	}
	defer cs.Close()
	y := airCellAbove(t, cs, x, 50, z)

	const placeReqID = uint32(52011)
	if err := actor.ctrl.SendCtrl(testutil.MsgSetBlockAction,
		placeAtCell(actor.playerID, x, y, z, stoneID, placeReqID)); err != nil {
		t.Fatalf("actor SetBlockAction: %v", err)
	}
	if _, err := actor.ctrl.WaitForBlockAck(placeReqID, Protocol.BlockAckStatusACCEPTED, 5*time.Second); err != nil {
		t.Fatalf("actor never got BlockAck: %v", err)
	}

	// Nothing but silence on the actor's bulk stream for this position.
	deadline := time.Now().Add(3 * time.Second)
	for {
		remaining := time.Until(deadline)
		if remaining <= 0 {
			return
		}
		msgType, data, err := actor.bulk.ReadCtrl(remaining)
		if err != nil {
			return
		}
		if msgType != testutil.MsgBlockUpdate {
			continue
		}
		ev := Protocol.GetRootAsBlockChangedEvent(data, 0)
		if ev == nil || ev.Pos(nil) == nil {
			continue
		}
		if ev.Pos(nil).X() == x && ev.Pos(nil).Y() == y && ev.Pos(nil).Z() == z {
			t.Fatalf("actor received an echo of its own change at (%d,%d,%d) — "+
				"source_player_id exclusion is broken", x, y, z)
		}
	}
}

// TestGateway_DisconnectDoesNotAffectOtherSession: sessions are independent. A
// singleton gateway tears down its single connection on every close, which is
// what used to make a second participant impossible.
func TestGateway_DisconnectDoesNotAffectOtherSession(t *testing.T) {
	const (
		cx = int32(41)
		cy = int32(1)
		cz = int32(41)
	)

	leaving := dialClientPair(t, gw)
	staying := dialClientPair(t, gw)
	defer staying.Close()

	leaving.Close()

	// The survivor can still request and still be served.
	if err := staying.ctrl.RequestChunk(staying.playerID, cx, cy, cz); err != nil {
		t.Fatalf("surviving session cannot send after the peer left: %v", err)
	}
	expectChunkAt(t, staying.bulk, cx, cy, cz, 15*time.Second)
}
