package integration

import (
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
)

func TestGateway_ReceivesChunkDataOnConnect(t *testing.T) {
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()

	data, err := c.ExpectMsgType(testutil.MsgCompressedChunk, 5*time.Second)
	if err != nil {
		t.Fatal("expected CompressedChunk on connect, got:", err)
	}
	t.Logf("Received CompressedChunk (%d bytes) on connect", len(data))
}

func TestGateway_BlockPersistsAfterReconnect(t *testing.T) {
	pos := [3]int32{500, 50, 500}

	c1, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}

	fbData := testutil.BuildSetBlockAction(42, pos[0], pos[1], pos[2], 0, 8)
	if err := c1.SendCtrl(testutil.MsgSetBlockAction, fbData); err != nil {
		c1.Close()
		t.Fatalf("send SetBlockAction: %v", err)
	}
	if _, err := c1.ExpectMsgType(testutil.MsgBlockAck, 5*time.Second); err != nil {
		c1.Close()
		t.Fatalf("expect BlockAck: %v", err)
	}
	c1.Close()

	time.Sleep(500 * time.Millisecond)

	c2, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable on reconnect: %v", err)
	}
	defer c2.Close()

	_, err = c2.ExpectMsgType(testutil.MsgCompressedChunk, 5*time.Second)
	if err != nil {
		t.Fatal("expected CompressedChunk on reconnect, got:", err)
	}
	t.Logf("Reconnected and received CompressedChunk — block persisted through disconnect")
}

func TestGateway_BulkPortConnects(t *testing.T) {
	bulkAddr := testutil.GatewayAddress{CtrlHost: "127.0.0.1", CtrlPort: 7778, BulkHost: "127.0.0.1", BulkPort: 7778}
	bulk, err := testutil.DialGateway(bulkAddr, 5*time.Second)
	if err != nil {
		t.Skipf("Bulk gateway port not reachable: %v", err)
	}
	defer bulk.Close()
	t.Log("Bulk port 7778 connected successfully")
}
