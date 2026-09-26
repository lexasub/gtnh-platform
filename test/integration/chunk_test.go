package integration

import (
	"testing"
	"time"

	flatbuffers "github.com/google/flatbuffers/go"
	"github.com/gtnh-platform/integration-tests/testutil"
	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
)

// stoneID is "0:0:1" packed (items.csv:8). The old literal 8 predates the
// packed-id scheme.
const stoneID uint16 = 1

func TestChunk_DataIntegrity(t *testing.T) {
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()

	playerID := uint64(42)
	pos := [3]int32{400, 50, 400}

	// Gateway's automatic CHUNK_REQUEST only covers the player's saved spawn
	// position, so prime this chunk or the CAS loses the world-gen race.
	if err := c.RequestChunk(playerID, pos[0]>>5, pos[1]>>5, pos[2]>>5); err != nil {
		t.Fatalf("request chunk: %v", err)
	}
	c.WaitForChunkGeneration(4 * time.Second)

	// A placement CASes against air, so target a verified empty cell. This
	// probe uses the typed client; the assertion below deliberately keeps the
	// hand-built GetBlockReq so the raw ChunkStore wire format stays covered.
	probe, err := testutil.DialChunkStore("127.0.0.1", 5001, 5*time.Second)
	if err != nil {
		t.Skipf("ChunkStore not reachable: %v", err)
	}
	defer probe.Close()
	placeY := airCellAbove(t, probe, pos[0], pos[1], pos[2])

	cs, err := testutil.DialRouter("127.0.0.1", 5001, 5*time.Second)
	if err != nil {
		t.Skipf("ChunkStore not reachable: %v", err)
	}
	defer cs.Close()

	const reqID = uint32(46001)
	if err := c.SendCtrl(testutil.MsgSetBlockAction,
		placeAtCell(playerID, pos[0], placeY, pos[2], stoneID, reqID)); err != nil {
		t.Fatalf("send SetBlockAction: %v", err)
	}
	if _, err := c.WaitForBlockAck(reqID, Protocol.BlockAckStatusACCEPTED, 5*time.Second); err != nil {
		t.Fatalf("expect BlockAck: %v", err)
	}
	// The ACK is optimistic; the commit is the ChunkStore write being queried.
	pos[1] = placeY

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

	// The ChunkStore service reads [4-byte BE length][1-byte type][FlatBuffer];
	// WriteFrame omits the type byte, so only WriteChunkStoreFrame is correct
	// here (testutil/chunkstore.go:41-48).
	if err := testutil.WriteChunkStoreFrame(cs.Conn(), b.FinishedBytes()); err != nil {
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

		if blockID == stoneID {
			break
		}
		// Block not committed yet — re-send request after short delay
		time.Sleep(200 * time.Millisecond)
		testutil.WriteChunkStoreFrame(cs.Conn(), b.FinishedBytes())
		respPayload, err = testutil.ReadFrameRaw(cs.Conn(), 5*time.Second)
		if err != nil {
			t.Fatalf("read from ChunkStore: %v", err)
		}
		if len(respPayload) < 1 {
			t.Fatal("response too short")
		}
		respPayload = respPayload[1:]
	}

	if blockID != stoneID {
		t.Errorf("expected block_id=%d (stone), got %d", stoneID, blockID)
	}
	t.Logf("ChunkStore: block_id=%d meta=%d mb_id=%d", blockID, meta, mbID)
}
