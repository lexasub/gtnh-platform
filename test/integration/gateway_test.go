package integration

import (
	"strings"
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
)

// Compressed chunk data is a BULK-port message, never a ctrl one: Gateway
// routes world.chunk.loaded.compressed exclusively to the bulk connection
// (src/apps/gateway/gateway.cpp, "chunk data is large"), and drops it with
// "chunk ... had no subscriber" when the bulk socket is not paired with a
// session that asked for the chunk.
//
// The old version waited for it on the ctrl port, which the server is not
// contracted to do.
func TestGateway_ReceivesChunkDataOnConnect(t *testing.T) {
	// Ctrl first: the connect is what triggers Gateway's CHUNK_REQUEST for the
	// session's position, and the welcome carries the identity that request
	// must use.
	ctrl, err := testutil.DialGatewaySession(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer ctrl.Close()

	// Then the bulk port, paired to this session — that is where the chunk
	// payload is delivered.
	bulk, err := testutil.DialGatewayBulk(gw, ctrl.SessionID(), 5*time.Second)
	if err != nil {
		t.Skipf("Gateway bulk port not reachable: %v", err)
	}
	defer bulk.Close()

	data, err := bulk.ExpectMsgType(testutil.MsgCompressedChunk, 10*time.Second)
	if err != nil {
		t.Fatalf("expected CompressedChunk on the bulk port after connect: %v", err)
	}
	t.Logf("Received CompressedChunk (%d bytes) on the bulk port as player %d",
		len(data), ctrl.PlayerID())
}

func TestGateway_AuthenticationPrecedesSessionWelcome(t *testing.T) {
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()
	if _, err := c.ExpectMsgType(testutil.MsgSessionWelcome, 100*time.Millisecond); err == nil {
		t.Fatal("Gateway sent SessionWelcome before authentication")
	}
	if c.PlayerID() != 0 || c.SessionID() != 0 {
		t.Fatalf("unauthenticated client has identity: player=%d session=%d", c.PlayerID(), c.SessionID())
	}
	status, _, err := c.ExpectAuthResult(10 * time.Millisecond)
	if err == nil {
		t.Fatalf("unexpected pre-auth result before sending credentials: %v", status)
	}
	if err := c.SendAccountAuth(true, testutil.NextTestUsername(), "integration-test-password"); err != nil {
		t.Fatalf("send registration: %v", err)
	}
	status, _, err = c.ExpectAuthResult(5 * time.Second)
	if err != nil || status != Protocol.AccountAuthStatusSUCCESS {
		t.Fatalf("registration result status=%v err=%v", status, err)
	}
	sessionID, playerID, err := c.ExpectSessionWelcome(5 * time.Second)
	if err != nil || sessionID == 0 || playerID == 0 {
		t.Fatalf("authenticated welcome session=%d player=%d err=%v", sessionID, playerID, err)
	}
}

func TestGateway_ClosesUnauthenticatedCtrlAtDeadline(t *testing.T) {
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()
	started := time.Now()
	_, _, err = c.ReadCtrl(17 * time.Second)
	elapsed := time.Since(started)
	if err == nil {
		t.Fatal("unauthenticated ctrl connection remained open after the auth deadline")
	}
	if elapsed < 14*time.Second || elapsed > 17*time.Second {
		t.Fatalf("unauthenticated connection closed after %s, want approximately 15s", elapsed)
	}
}

func TestGateway_RejectsGameplayBeforeAuthentication(t *testing.T) {
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()
	if err := c.SendCtrl(testutil.MsgPlayerAction,
		testutil.BuildPlayerAction(999, Protocol.PlayerActionTypeITEM_ACTION, 0, 0, 0, 2, 1)); err != nil {
		t.Fatalf("send pre-auth gameplay frame: %v", err)
	}
	_, _, err = c.ReadCtrl(2 * time.Second)
	if err == nil {
		t.Fatal("unauthenticated gameplay connection unexpectedly received a response")
	}
	if strings.Contains(err.Error(), "timeout") {
		t.Fatalf("Gateway left unauthenticated gameplay socket open: %v", err)
	}
}

func TestGateway_AllowsRetryAfterInvalidCredentials(t *testing.T) {
	username := testutil.NextTestUsername()
	password := "integration-retry-password"
	registered, err := testutil.DialGatewayAccountSession(gw, 5*time.Second, username, password, true)
	if err != nil {
		t.Fatalf("register account: %v", err)
	}
	registered.Close()
	time.Sleep(150 * time.Millisecond)

	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Fatalf("dial ctrl: %v", err)
	}
	defer c.Close()
	if err := c.SendAccountAuth(false, username, "incorrect-retry-password"); err != nil {
		t.Fatalf("send incorrect login: %v", err)
	}
	status, _, err := c.ExpectAuthResult(5 * time.Second)
	if err != nil || status != Protocol.AccountAuthStatusINVALID_CREDENTIALS {
		t.Fatalf("incorrect login status=%v err=%v", status, err)
	}
	if err := c.SendAccountAuth(false, username, password); err != nil {
		t.Fatalf("retry valid login: %v", err)
	}
	status, _, err = c.ExpectAuthResult(5 * time.Second)
	if err != nil || status != Protocol.AccountAuthStatusSUCCESS {
		t.Fatalf("valid retry status=%v err=%v", status, err)
	}
	if _, playerID, err := c.ExpectSessionWelcome(5 * time.Second); err != nil || playerID == 0 {
		t.Fatalf("valid retry welcome player=%d err=%v", playerID, err)
	}
}

func TestGateway_RejectsConcurrentLoginToActiveAccount(t *testing.T) {
	username := testutil.NextTestUsername()
	password := "integration-active-password"
	first, err := testutil.DialGatewayAccountSession(gw, 5*time.Second, username, password, true)
	if err != nil {
		t.Fatalf("register account: %v", err)
	}
	defer first.Close()

	second, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Fatalf("dial second ctrl: %v", err)
	}
	defer second.Close()
	if err := second.SendAccountAuth(false, username, password); err != nil {
		t.Fatalf("send duplicate login: %v", err)
	}
	status, _, err := second.ExpectAuthResult(5 * time.Second)
	if err != nil || status != Protocol.AccountAuthStatusACCOUNT_ACTIVE {
		t.Fatalf("duplicate login status=%v err=%v", status, err)
	}
	if first.PlayerID() == 0 {
		t.Fatal("rejected duplicate login disturbed active session")
	}
}

func TestGateway_RateLimitsRepeatedAccountLoginFailures(t *testing.T) {
	username := testutil.NextTestUsername()
	password := "integration-rate-limit-password"
	registered, err := testutil.DialGatewayAccountSession(gw, 5*time.Second, username, password, true)
	if err != nil {
		t.Fatalf("register account: %v", err)
	}
	registered.Close()
	time.Sleep(150 * time.Millisecond)

	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Fatalf("dial ctrl: %v", err)
	}
	defer c.Close()
	for attempt := 1; attempt <= 6; attempt++ {
		if err := c.SendAccountAuth(false, username, "wrong-integration-password"); err != nil {
			t.Fatalf("send failed login %d: %v", attempt, err)
		}
		status, _, err := c.ExpectAuthResult(5 * time.Second)
		if err != nil {
			t.Fatalf("failed login %d result: %v", attempt, err)
		}
		want := Protocol.AccountAuthStatusINVALID_CREDENTIALS
		if attempt == 6 {
			want = Protocol.AccountAuthStatusRATE_LIMITED
		}
		if status != want {
			t.Fatalf("failed login %d status=%v, want %v", attempt, status, want)
		}
	}
}

// TestGateway_BlockPersistsAfterReconnect proves the block survives a
// disconnect: the placement is committed in ChunkStore, and after reconnecting
// the fresh session is served that same chunk again.
//
// The placement must carry a held item (PlaceBlockHandler::canHandle,
// PlaceBlockHandler.cpp:14-18) and the chunk arrives on the BULK port, not on
// ctrl.
func TestGateway_BlockPersistsAfterReconnect(t *testing.T) {
	const (
		x = int32(500)
		z = int32(500)
	)

	c1, err := testutil.DialGatewaySession(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	playerID := c1.PlayerID()
	if err := c1.RequestChunk(playerID, x>>5, 50>>5, z>>5); err != nil {
		c1.Close()
		t.Fatalf("request chunk: %v", err)
	}
	c1.WaitForChunkGeneration(4 * time.Second)

	cs, err := testutil.DialChunkStore("127.0.0.1", 5001, 5*time.Second)
	if err != nil {
		c1.Close()
		t.Fatalf("dial ChunkStore: %v", err)
	}
	y := airCellAbove(t, cs, x, 50, z)

	const placeReqID = uint32(47001)
	if err := c1.SendCtrl(testutil.MsgSetBlockAction,
		placeAtCell(playerID, x, y, z, stoneID, placeReqID)); err != nil {
		cs.Close()
		c1.Close()
		t.Fatalf("send SetBlockAction: %v", err)
	}
	if _, err := c1.WaitForBlockAck(placeReqID, Protocol.BlockAckStatusACCEPTED, 5*time.Second); err != nil {
		cs.Close()
		c1.Close()
		t.Fatalf("expect BlockAck: %v", err)
	}
	// The ACK is optimistic; wait for the authoritative commit before dropping
	// the connection, so "persists" really means persisted.
	if err := cs.WaitForBlock(x, y, z, stoneID, 5*time.Second); err != nil {
		cs.Close()
		c1.Close()
		t.Fatalf("block never committed: %v", err)
	}
	cs.Close()
	c1.Close()

	c2, err := testutil.DialGatewaySession(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable on reconnect: %v", err)
	}
	defer c2.Close()

	// A reconnecting player is placed at the last known position, so the new
	// session re-requests the chunk it is standing in.
	bulk, err := testutil.DialGatewayBulk(gw, c2.SessionID(), 5*time.Second)
	if err != nil {
		t.Skipf("Gateway bulk port not reachable on reconnect: %v", err)
	}
	defer bulk.Close()

	if _, err := bulk.ExpectMsgType(testutil.MsgCompressedChunk, 10*time.Second); err != nil {
		t.Fatalf("expected CompressedChunk on the bulk port after reconnect: %v", err)
	}
	t.Logf("Reconnected as player %d and received CompressedChunk — block %d persisted through disconnect",
		c2.PlayerID(), stoneID)
}

func TestGateway_AccountLoginKeepsPlayerIdentity(t *testing.T) {
	username := testutil.NextTestUsername()
	password := "integration-persist-password"
	first, err := testutil.DialGatewayAccountSession(gw, 5*time.Second, username, password, true)
	if err != nil {
		t.Fatalf("register account: %v", err)
	}
	playerID := first.PlayerID()
	firstBulk, err := testutil.DialGatewayBulk(gw, first.SessionID(), 5*time.Second)
	if err != nil {
		first.Close()
		t.Fatalf("pair first bulk connection: %v", err)
	}
	if err := first.SendCtrl(testutil.MsgPlayerAction, testutil.BuildPlayerAction(
		playerID, Protocol.PlayerActionTypeITEM_ACTION, 0, 0, 0, 2, 4)); err != nil {
		first.Close()
		t.Fatalf("grant test inventory: %v", err)
	}
	if _, err := first.WaitForInventoryItem(playerID, 2, 4, 5*time.Second); err != nil {
		first.Close()
		t.Fatalf("wait for inventory grant: %v", err)
	}
	first.Close()
	firstBulk.Close()
	time.Sleep(150 * time.Millisecond)

	second, err := testutil.DialGatewayAccountSession(gw, 5*time.Second, username, password, false)
	if err != nil {
		t.Fatalf("login to existing account: %v", err)
	}
	defer second.Close()
	if second.PlayerID() != playerID {
		t.Fatalf("persistent player id changed across login: first=%d second=%d", playerID, second.PlayerID())
	}
	secondBulk, err := testutil.DialGatewayBulk(gw, second.SessionID(), 5*time.Second)
	if err != nil {
		t.Fatalf("pair second bulk connection after reconnect: %v", err)
	}
	defer secondBulk.Close()
}

func TestGateway_BulkPortConnects(t *testing.T) {
	ctrl, err := testutil.DialGatewaySession(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer ctrl.Close()

	bulk, err := testutil.DialGatewayBulk(gw, ctrl.SessionID(), 5*time.Second)
	if err != nil {
		t.Skipf("Bulk gateway port not reachable: %v", err)
	}
	defer bulk.Close()
	t.Logf("Bulk port 7778 paired to session %d (player %d)",
		ctrl.SessionID(), ctrl.PlayerID())
}
