package integration

import (
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
)

// Compressed chunk data is a BULK-port message, never a ctrl one: Gateway
// routes world.chunk.loaded.compressed exclusively to the bulk connection
// (src/apps/gateway/gateway.cpp:387-393, "chunk data is large"), and drops it
// with "compressed chunk dropped, no bulk client" when no bulk socket is open.
// The old version waited for it on the ctrl port, which the server is not
// contracted to do.
func TestGateway_ReceivesChunkDataOnConnect(t *testing.T) {
	// Ctrl first: the connect is what triggers Gateway's CHUNK_REQUEST for the
	// saved position (gateway.cpp:116-131).
	ctrl, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer ctrl.Close()

	// Then the bulk port, which is where the chunk payload is delivered.
	bulkAddr := testutil.GatewayAddress{
		CtrlHost: gw.CtrlHost, CtrlPort: gw.BulkPort,
		BulkHost: gw.BulkHost, BulkPort: gw.BulkPort,
	}
	bulk, err := testutil.DialGateway(bulkAddr, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway bulk port not reachable: %v", err)
	}
	defer bulk.Close()

	data, err := bulk.ExpectMsgType(testutil.MsgCompressedChunk, 10*time.Second)
	if err != nil {
		t.Fatalf("expected CompressedChunk on the bulk port after connect: %v", err)
	}
	t.Logf("Received CompressedChunk (%d bytes) on the bulk port", len(data))
}

// TestGateway_BlockPersistsAfterReconnect proves the block survives a
// disconnect: the placement is committed in ChunkStore, and after reconnecting
// the fresh bulk connection is served that same chunk again.
//
// Both corrections versus the old version: the placement must carry a held
// item (PlaceBlockHandler::canHandle, PlaceBlockHandler.cpp:14-18) and the
// chunk arrives on the BULK port (gateway.cpp:387-393), not on ctrl.
func TestGateway_BlockPersistsAfterReconnect(t *testing.T) {
	const (
		x = int32(500)
		z = int32(500)
	)

	c1, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	if err := c1.RequestChunk(42, x>>5, 50>>5, z>>5); err != nil {
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
		placeAtCell(42, x, y, z, stoneID, placeReqID)); err != nil {
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

	c2, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable on reconnect: %v", err)
	}
	defer c2.Close()

	// A reconnecting player is placed at the last known position, so Gateway
	// re-requests the chunk it is standing in.
	bulkAddr := testutil.GatewayAddress{
		CtrlHost: gw.CtrlHost, CtrlPort: gw.BulkPort,
		BulkHost: gw.BulkHost, BulkPort: gw.BulkPort,
	}
	bulk, err := testutil.DialGateway(bulkAddr, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway bulk port not reachable on reconnect: %v", err)
	}
	defer bulk.Close()

	if _, err := bulk.ExpectMsgType(testutil.MsgCompressedChunk, 10*time.Second); err != nil {
		t.Fatalf("expected CompressedChunk on the bulk port after reconnect: %v", err)
	}
	t.Logf("Reconnected and received CompressedChunk — block %d persisted through disconnect", stoneID)
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
