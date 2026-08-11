package integration

import (
	"testing"
	"time"

	flatbuffers "github.com/google/flatbuffers/go"
	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
	"github.com/gtnh-platform/integration-tests/testutil"
)

func TestChunk_DataIntegrity(t *testing.T) {
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()

	playerID := uint64(42)
	pos := [3]int32{400, 50, 400}

	fbData := testutil.BuildSetBlockAction(playerID, pos[0], pos[1], pos[2], 0, 8)
	if err := c.SendCtrl(testutil.MsgSetBlockAction, fbData); err != nil {
		t.Fatalf("send SetBlockAction: %v", err)
	}
	data, err := c.ExpectMsgType(testutil.MsgBlockAck, 5*time.Second)
	if err != nil {
		t.Fatalf("expect BlockAck: %v", err)
	}
	ack := Protocol.GetRootAsBlockAck(data, 0)
	if ack.Status() != Protocol.BlockAckStatusACCEPTED {
		t.Fatalf("block placement failed: %v", ack.Status())
	}

	cs, err := testutil.DialRouter("127.0.0.1", 5001, 5*time.Second)
	if err != nil {
		t.Skipf("ChunkStore not reachable: %v", err)
	}
	defer cs.Close()

	b := flatbuffers.NewBuilder(32)
	blockPos := Protocol.CreateVec3i(b, pos[0], pos[1], pos[2])
	Protocol.GetBlockReqStart(b)
	Protocol.GetBlockReqAddPos(b, blockPos)
	req := Protocol.GetBlockReqEnd(b)
	Protocol.ChunkStoreMessageStart(b)
	Protocol.ChunkStoreMessageAddReqId(b, 1)
	Protocol.ChunkStoreMessageAddRequestType(b, Protocol.ChunkStoreRequestGetBlockReq)
	Protocol.ChunkStoreMessageAddRequest(b, req)
	msg := Protocol.ChunkStoreMessageEnd(b)
	Protocol.ChunkStoreFrameStart(b)
	Protocol.ChunkStoreFrameAddPayloadType(b, Protocol.ChunkStorePayloadChunkStoreMessage)
	Protocol.ChunkStoreFrameAddPayload(b, msg)
	frame := Protocol.ChunkStoreFrameEnd(b)
	b.Finish(frame)

	if err := testutil.WriteFrame(cs.Conn(), b.FinishedBytes()); err != nil {
		t.Fatalf("write to ChunkStore: %v", err)
	}

	respPayload, err := testutil.ReadFrameRaw(cs.Conn(), 5*time.Second)
	if err != nil {
		t.Fatalf("read from ChunkStore: %v", err)
	}

	// C++ EnqueueWrite prepends 1-byte msg_type (always 0) — skip it
	if len(respPayload) < 1 {
		t.Fatal("response too short")
	}
	respPayload = respPayload[1:]

	// Retry parsing the response, retrying if block not yet committed
	var (
		blockID uint16
		meta    uint8
		mbID    uint32
	)
	queryDeadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(queryDeadline) {
		respFrame := Protocol.GetRootAsChunkStoreFrame(respPayload, 0)
		var replyTable flatbuffers.Table
		respFrame.Payload(&replyTable)
		reply := new(Protocol.ChunkStoreReply)
		reply.Init(replyTable.Bytes, replyTable.Pos)

		if reply.ResponseType() != Protocol.ChunkStoreResponseGetBlockResp {
			t.Fatalf("expected GetBlockResp, got %v", reply.ResponseType())
		}

		var respTable flatbuffers.Table
		reply.Response(&respTable)
		getResp := new(Protocol.GetBlockResp)
		getResp.Init(respTable.Bytes, respTable.Pos)

		blockID = getResp.BlockId()
		meta = getResp.Meta()
		mbID = getResp.MbId()

		if blockID == 8 {
			break
		}
		// Block not committed yet — re-send request after short delay
		time.Sleep(200 * time.Millisecond)
		testutil.WriteFrame(cs.Conn(), b.FinishedBytes())
		respPayload, err = testutil.ReadFrameRaw(cs.Conn(), 5*time.Second)
		if err != nil {
			t.Fatalf("read from ChunkStore: %v", err)
		}
		if len(respPayload) < 1 {
			t.Fatal("response too short")
		}
		respPayload = respPayload[1:]
	}

	if blockID != 8 {
		t.Errorf("expected block_id=8 (stone), got %d", blockID)
	}
	t.Logf("ChunkStore: block_id=%d meta=%d mb_id=%d", blockID, meta, mbID)
}
