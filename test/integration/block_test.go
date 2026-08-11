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

func TestSetBlock_CAS_Accept(t *testing.T) {
	c := connect(t)
	defer c.Close()

	fbData := testutil.BuildSetBlockAction(42, 100, 50, 200, 0, 1)
	if err := c.SendCtrl(testutil.MsgSetBlockAction, fbData); err != nil {
		t.Fatalf("send SetBlockAction: %v", err)
	}
	data, err := c.ExpectMsgType(testutil.MsgBlockAck, 5*time.Second)
	if err != nil {
		t.Fatalf("expect BlockAck: %v", err)
	}
	testutil.AssertBlockAck(t, data, Protocol.BlockAckStatusACCEPTED)
}

func TestSetBlock_CAS_Conflict(t *testing.T) {
	// First place a block via a separate connection, wait for it to settle
	{
		c := connect(t)
		fbData := testutil.BuildSetBlockAction(42, 110, 50, 210, 0, 1)
		c.SendCtrl(testutil.MsgSetBlockAction, fbData)
		c.ExpectMsgType(testutil.MsgBlockAck, 5*time.Second)
		c.Close()
	}
	time.Sleep(1 * time.Second) // let ChunkStore commit

	c := connect(t)
	defer c.Close()

	// Same position, stale expected=0 → CONFLICT
	fbData := testutil.BuildSetBlockAction(42, 110, 50, 210, 0, 2)
	if err := c.SendCtrl(testutil.MsgSetBlockAction, fbData); err != nil {
		t.Fatalf("send SetBlockAction: %v", err)
	}

	// SimCore sends optimistic ACCEPTED first, then CONFLICT after ChunkStore CAS
	deadline := time.Now().Add(5 * time.Second)
	var gotConflict bool
	for time.Now().Before(deadline) {
		data, err := c.ExpectMsgType(testutil.MsgBlockAck, 1*time.Second)
		if err != nil {
			continue
		}
		ack := Protocol.GetRootAsBlockAck(data, 0)
		if ack.Status() == Protocol.BlockAckStatusCONFLICT {
			gotConflict = true
			break
		}
	}
	if !gotConflict {
		t.Fatal("expected CONFLICT, never received it")
	}
}

func TestSetBlock_CAS_AcceptAfterCorrectExpected(t *testing.T) {
	// Place a block first
	{
		c := connect(t)
		fbData := testutil.BuildSetBlockAction(42, 120, 50, 220, 0, 5)
		c.SendCtrl(testutil.MsgSetBlockAction, fbData)
		c.ExpectMsgType(testutil.MsgBlockAck, 5*time.Second)
		c.Close()
	}
	time.Sleep(1 * time.Second)

	c := connect(t)
	defer c.Close()

	// Replace with correct expected block_id=5, new block_id=7
	fbData := testutil.BuildSetBlockAction(42, 120, 50, 220, 5, 7)
	if err := c.SendCtrl(testutil.MsgSetBlockAction, fbData); err != nil {
		t.Fatalf("send SetBlockAction: %v", err)
	}
	data, err := c.ExpectMsgType(testutil.MsgBlockAck, 5*time.Second)
	if err != nil {
		t.Fatalf("expect BlockAck: %v", err)
	}
	ack := Protocol.GetRootAsBlockAck(data, 0)
	if ack.Status() != Protocol.BlockAckStatusACCEPTED {
		t.Errorf("expected ACCEPTED, got %v", ack.Status())
	}
}
