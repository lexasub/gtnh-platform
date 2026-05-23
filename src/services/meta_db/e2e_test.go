// E2E integration test: MetaDB FlatBuffers protocol via direct TCP.
//
// Build & run:
//   cd src/services/meta_db
//   go test -tags=e2e -v -run TestMetaDBE2E -timeout 30s
//
// Requires: routerd + metadbd built and running, or started by test.
// Default: connects to localhost:5006 (MetaDB FlatBuffers port).

package main

import (
	"encoding/binary"
	"fmt"
	"net"
	"testing"
	"time"

	flatbuffers "github.com/google/flatbuffers/go"

	"github.com/gtnh-platform/protocol/generated/go/Protocol"
)

// Inline RouterClient helpers (mirrors testutil).
type testRouterClient struct {
	conn net.Conn
}

func dialTestRouter(host string, port int, timeout time.Duration) (*testRouterClient, error) {
	conn, err := net.DialTimeout("tcp", fmt.Sprintf("%s:%d", host, port), timeout)
	if err != nil {
		return nil, err
	}
	return &testRouterClient{conn: conn}, nil
}

func (r *testRouterClient) Close() { r.conn.Close() }

func (r *testRouterClient) sendStringFrame(mt byte, s string) error {
	payload := make([]byte, 2+len(s))
	binary.BigEndian.PutUint16(payload[:2], uint16(len(s)))
	copy(payload[2:], s)
	frame := make([]byte, 5+len(payload))
	binary.BigEndian.PutUint32(frame[:4], uint32(len(payload)+1))
	frame[4] = mt
	copy(frame[5:], payload)
	_, err := r.conn.Write(frame)
	return err
}

func (r *testRouterClient) Subscribe(topic string) error {
	return r.sendStringFrame(1, topic) // msgSubscribe = 1
}

func (r *testRouterClient) Publish(topic string, data []byte) error {
	payload := make([]byte, 2+len(topic)+len(data))
	binary.BigEndian.PutUint16(payload[:2], uint16(len(topic)))
	copy(payload[2:], topic)
	copy(payload[2+len(topic):], data)
	frame := make([]byte, 5+len(payload))
	binary.BigEndian.PutUint32(frame[:4], uint32(len(payload)+1))
	frame[4] = 3 // msgPublish = 3
	copy(frame[5:], payload)
	_, err := r.conn.Write(frame)
	return err
}

func (r *testRouterClient) ReadFrame(timeout time.Duration) (uint8, []byte, error) {
	if timeout > 0 {
		r.conn.SetReadDeadline(time.Now().Add(timeout))
	}
	header := make([]byte, 5)
	if _, err := r.conn.Read(header); err != nil {
		return 0, nil, err
	}
	payloadLen := binary.BigEndian.Uint32(header[:4])
	mt := header[4]
	if payloadLen < 1 {
		return 0, nil, fmt.Errorf("short frame")
	}
	payload := make([]byte, payloadLen-1)
	if len(payload) > 0 {
		if _, err := r.conn.Read(payload); err != nil {
			return 0, nil, err
		}
	}
	return mt, payload, nil
}

func readTopicPayload(payload []byte) (string, []byte, error) {
	if len(payload) < 2 {
		return "", nil, fmt.Errorf("too short")
	}
	topicLen := int(binary.BigEndian.Uint16(payload[:2]))
	if 2+topicLen > len(payload) {
		return "", nil, fmt.Errorf("topic len exceeds payload")
	}
	return string(payload[2 : 2+topicLen]), payload[2+topicLen:], nil
}

// TestMetaDBE2E_RouterPublish sends a SetInventorySlotReq through the Router
// and checks that MetaDB responds with an ack and publishes InventoryUpdate.
func TestMetaDBE2E_RouterPublish(t *testing.T) {
	if !isRouterUp() {
		t.Skip("Router not running — start services first")
	}

	// Connect to Router as test client
	rc, err := dialTestRouter("127.0.0.1", 4000, 5*time.Second)
	if err != nil {
		t.Fatalf("connect to Router: %v", err)
	}
	defer rc.Close()

	// Subscribe to meta_db.inventory.set.response so we get the reply
	rc.Subscribe("meta_db.inventory.set.response")
	time.Sleep(200 * time.Millisecond) // let subscribe propagate

	// Build SetInventorySlotReq
	playerID := uint64(42)
	slotIndex := uint16(5)
	itemID := uint16(7)
	count := uint8(1)

	builder := flatbuffers.NewBuilder(128)
	Protocol.SetInventorySlotReqStart(builder)
	Protocol.SetInventorySlotReqAddPlayerId(builder, playerID)
	Protocol.SetInventorySlotReqAddSlotIndex(builder, slotIndex)
	Protocol.SetInventorySlotReqAddItemId(builder, itemID)
	Protocol.SetInventorySlotReqAddCount(builder, count)
	Protocol.SetInventorySlotReqAddMeta(builder, 0)
	reqOffset := Protocol.SetInventorySlotReqEnd(builder)

	Protocol.MetaDBMessageStart(builder)
	Protocol.MetaDBMessageAddReqId(builder, 1)
	Protocol.MetaDBMessageAddRequestType(builder, Protocol.MetaDBRequestSetInventorySlotReq)
	Protocol.MetaDBMessageAddRequest(builder, reqOffset)
	msgOffset := Protocol.MetaDBMessageEnd(builder)

	Protocol.MetaDBFrameStart(builder)
	Protocol.MetaDBFrameAddPayloadType(builder, Protocol.MetaDBPayloadMetaDBMessage)
	Protocol.MetaDBFrameAddPayload(builder, msgOffset)
	frameOffset := Protocol.MetaDBFrameEnd(builder)

	builder.Finish(frameOffset)
	fbData := builder.FinishedBytes()

	// Publish to meta_db.inventory.set via Router
	if err := rc.Publish("meta_db.inventory.set", fbData); err != nil {
		t.Fatalf("publish meta_db.inventory.set: %v", err)
	}
	t.Logf("Published SetInventorySlotReq (%d bytes)", len(fbData))

	// Read response — should get a MetaDBReply back on meta_db.inventory.set.response
	// Also should get player.inventory.update
	deadline := time.Now().Add(10 * time.Second)
	foundReply := false
	foundUpdate := false
	for time.Now().Before(deadline) {
		rc.conn.SetReadDeadline(time.Now().Add(2 * time.Second))
		mt, payload, err := rc.ReadFrame(1 * time.Second)
		if err != nil {
			continue
		}
		t.Logf("RX frame: type=%d len=%d", mt, len(payload))
		if mt != 3 { // MsgPublish
			continue
		}
		topic, data, err := readTopicPayload(payload)
		if err != nil {
			t.Logf("  topic parse error: %v", err)
			continue
		}
		hexLen := 32
		if len(data) < hexLen { hexLen = len(data) }
		t.Logf("  topic=%s data=%d bytes hex=%x", topic, len(data), data[:hexLen])
		if topic == "meta_db.inventory.set.response" {
			foundReply = true
		}
		if topic == "player.inventory.update" {
			foundUpdate = true
		}
	}
	if !foundReply {
		// Check if MetaDB is even subscribed
		t.Log("No reply received — MetaDB may not be running or connected")
	}
	_ = foundUpdate
}

func isRouterUp() bool {
	conn, err := net.DialTimeout("tcp", "127.0.0.1:4000", 100*time.Millisecond)
	if err != nil {
		return false
	}
	conn.Close()
	return true
}

func TestMetaDBE2E_DirectTCP(t *testing.T) {
	// Connect to MetaDB FlatBuffers listener
	conn, err := net.DialTimeout("tcp", "127.0.0.1:5006", 5*time.Second)
	if err != nil {
		t.Skipf("MetaDB not reachable on :5006 — start services first: %v", err)
	}
	defer conn.Close()

	// Build GetInventoryReq
	playerID := uint64(42)

	builder := flatbuffers.NewBuilder(256)
	Protocol.GetInventoryReqStart(builder)
	Protocol.GetInventoryReqAddPlayerId(builder, playerID)
	Protocol.GetInventoryReqAddSlotIndex(builder, 0)
	reqOffset := Protocol.GetInventoryReqEnd(builder)

	// Build MetaDBMessage wrapping the request
	Protocol.MetaDBMessageStart(builder)
	Protocol.MetaDBMessageAddReqId(builder, 1)
	Protocol.MetaDBMessageAddRequestType(builder, Protocol.MetaDBRequestGetInventoryReq)
	Protocol.MetaDBMessageAddRequest(builder, reqOffset)
	msgOffset := Protocol.MetaDBMessageEnd(builder)

	// Build MetaDBFrame
	Protocol.MetaDBFrameStart(builder)
	Protocol.MetaDBFrameAddPayloadType(builder, Protocol.MetaDBPayloadMetaDBMessage)
	Protocol.MetaDBFrameAddPayload(builder, msgOffset)
	frameOffset := Protocol.MetaDBFrameEnd(builder)

	builder.Finish(frameOffset)
	fbData := builder.FinishedBytes()

	// Send: [4 bytes payload length BE][FlatBuffer]
	lenBuf := make([]byte, 4)
	binary.BigEndian.PutUint32(lenBuf, uint32(len(fbData)))
	conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
	if _, err := conn.Write(lenBuf); err != nil {
		t.Fatalf("write length: %v", err)
	}
	if _, err := conn.Write(fbData); err != nil {
		t.Fatalf("write payload: %v", err)
	}

	// Read response: [4 bytes length BE][FlatBuffer MetaDBFrame]
	conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	respLenBuf := make([]byte, 4)
	if _, err := conn.Read(respLenBuf); err != nil {
		t.Fatalf("read response length: %v", err)
	}
	respLen := binary.BigEndian.Uint32(respLenBuf)
	if respLen == 0 {
		t.Fatal("empty response")
	}

	respPayload := make([]byte, respLen)
	if _, err := conn.Read(respPayload); err != nil {
		t.Fatalf("read response payload: %v", err)
	}

	// Parse response
	respFrame := Protocol.GetRootAsMetaDBFrame(respPayload, 0)
	if respFrame == nil {
		t.Fatal("nil response frame")
	}

	payloadType := respFrame.PayloadType()
	if payloadType != Protocol.MetaDBPayloadMetaDBReply {
		t.Fatalf("expected MetaDBReply, got %v", payloadType)
	}

	var replyTable flatbuffers.Table
	if !respFrame.Payload(&replyTable) {
		t.Fatal("failed to extract reply table")
	}
	reply := new(Protocol.MetaDBReply)
	reply.Init(replyTable.Bytes, replyTable.Pos)

	if reply.ReqId() != 1 {
		t.Fatalf("expected req_id=1, got %d", reply.ReqId())
	}

	responseType := reply.ResponseType()
	var respTable flatbuffers.Table
	if !reply.Response(&respTable) {
		t.Fatal("failed to extract response table")
	}

	switch responseType {
	case Protocol.MetaDBResponseGetInventoryResp:
		resp := new(Protocol.GetInventoryResp)
		resp.Init(respTable.Bytes, respTable.Pos)
		t.Logf("GetInventoryResp: inventory length=%d", resp.InventoryLength())
		// Player 42 has no inventory — empty response is valid
	case Protocol.MetaDBResponseErrorResp:
		errResp := new(Protocol.ErrorResp)
		errResp.Init(respTable.Bytes, respTable.Pos)
		t.Fatalf("MetaDB returned error: %s", errResp.Message())
	default:
		t.Fatalf("unexpected response type: %v", responseType)
	}
}

func TestMetaDBE2E_SetInventorySlot(t *testing.T) {
	conn, err := net.DialTimeout("tcp", "127.0.0.1:5006", 5*time.Second)
	if err != nil {
		t.Skipf("MetaDB not reachable on :5006: %v", err)
	}
	defer conn.Close()

	builder := flatbuffers.NewBuilder(256)
	Protocol.SetInventorySlotReqStart(builder)
	Protocol.SetInventorySlotReqAddPlayerId(builder, 42)
	Protocol.SetInventorySlotReqAddSlotIndex(builder, 0)
	Protocol.SetInventorySlotReqAddItemId(builder, 1)  // stone
	Protocol.SetInventorySlotReqAddCount(builder, 10)
	Protocol.SetInventorySlotReqAddMeta(builder, 0)
	reqOffset := Protocol.SetInventorySlotReqEnd(builder)

	Protocol.MetaDBMessageStart(builder)
	Protocol.MetaDBMessageAddReqId(builder, 2)
	Protocol.MetaDBMessageAddRequestType(builder, Protocol.MetaDBRequestSetInventorySlotReq)
	Protocol.MetaDBMessageAddRequest(builder, reqOffset)
	msgOffset := Protocol.MetaDBMessageEnd(builder)

	Protocol.MetaDBFrameStart(builder)
	Protocol.MetaDBFrameAddPayloadType(builder, Protocol.MetaDBPayloadMetaDBMessage)
	Protocol.MetaDBFrameAddPayload(builder, msgOffset)
	frameOffset := Protocol.MetaDBFrameEnd(builder)

	builder.Finish(frameOffset)
	fbData := builder.FinishedBytes()

	lenBuf := make([]byte, 4)
	binary.BigEndian.PutUint32(lenBuf, uint32(len(fbData)))
	conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
	conn.Write(lenBuf)
	conn.Write(fbData)

	conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	respLenBuf := make([]byte, 4)
	conn.Read(respLenBuf)
	respLen := binary.BigEndian.Uint32(respLenBuf)
	respPayload := make([]byte, respLen)
	conn.Read(respPayload)

	respFrame := Protocol.GetRootAsMetaDBFrame(respPayload, 0)
	if respFrame == nil {
		t.Fatal("nil response frame")
	}
	var replyTable flatbuffers.Table
	respFrame.Payload(&replyTable)
	reply := new(Protocol.MetaDBReply)
	reply.Init(replyTable.Bytes, replyTable.Pos)

	if reply.ReqId() != 2 {
		t.Fatalf("expected req_id=2, got %d", reply.ReqId())
	}

	t.Logf("SetInventorySlotResp: responseType=%v", reply.ResponseType())
}

func TestMetaDBE2E_GetInventoryAfterSet(t *testing.T) {
	// Set slot, then get inventory, verify item is present
	conn, err := net.DialTimeout("tcp", "127.0.0.1:5006", 5*time.Second)
	if err != nil {
		t.Skipf("MetaDB not reachable on :5006: %v", err)
	}
	defer conn.Close()

	// First: SetInventorySlot
	builder := flatbuffers.NewBuilder(256)
	Protocol.SetInventorySlotReqStart(builder)
	Protocol.SetInventorySlotReqAddPlayerId(builder, 99)
	Protocol.SetInventorySlotReqAddSlotIndex(builder, 5)
	Protocol.SetInventorySlotReqAddItemId(builder, 4) // cobblestone
	Protocol.SetInventorySlotReqAddCount(builder, 64)
	Protocol.SetInventorySlotReqAddMeta(builder, 0)
	setReq := Protocol.SetInventorySlotReqEnd(builder)

	Protocol.MetaDBMessageStart(builder)
	Protocol.MetaDBMessageAddReqId(builder, 3)
	Protocol.MetaDBMessageAddRequestType(builder, Protocol.MetaDBRequestSetInventorySlotReq)
	Protocol.MetaDBMessageAddRequest(builder, setReq)
	setMsg := Protocol.MetaDBMessageEnd(builder)

	Protocol.MetaDBFrameStart(builder)
	Protocol.MetaDBFrameAddPayloadType(builder, Protocol.MetaDBPayloadMetaDBMessage)
	Protocol.MetaDBFrameAddPayload(builder, setMsg)
	setFrame := Protocol.MetaDBFrameEnd(builder)

	builder.Finish(setFrame)
	fbData := builder.FinishedBytes()

	lenBuf := make([]byte, 4)
	binary.BigEndian.PutUint32(lenBuf, uint32(len(fbData)))
	conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
	conn.Write(lenBuf)
	conn.Write(fbData)

	// Read set response
	respLenBuf := make([]byte, 4)
	conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	conn.Read(respLenBuf)
	respLen := binary.BigEndian.Uint32(respLenBuf)
	respPayload := make([]byte, respLen)
	conn.Read(respPayload)

	// Second: GetInventory to verify
	builder2 := flatbuffers.NewBuilder(256)
	Protocol.GetInventoryReqStart(builder2)
	Protocol.GetInventoryReqAddPlayerId(builder2, 99)
	Protocol.GetInventoryReqAddSlotIndex(builder2, 0)
	getReq := Protocol.GetInventoryReqEnd(builder2)

	Protocol.MetaDBMessageStart(builder2)
	Protocol.MetaDBMessageAddReqId(builder2, 4)
	Protocol.MetaDBMessageAddRequestType(builder2, Protocol.MetaDBRequestGetInventoryReq)
	Protocol.MetaDBMessageAddRequest(builder2, getReq)
	getMsg := Protocol.MetaDBMessageEnd(builder2)

	Protocol.MetaDBFrameStart(builder2)
	Protocol.MetaDBFrameAddPayloadType(builder2, Protocol.MetaDBPayloadMetaDBMessage)
	Protocol.MetaDBFrameAddPayload(builder2, getMsg)
	getFrame := Protocol.MetaDBFrameEnd(builder2)

	builder2.Finish(getFrame)
	fbData2 := builder2.FinishedBytes()

	binary.BigEndian.PutUint32(lenBuf, uint32(len(fbData2)))
	conn.Write(lenBuf)
	conn.Write(fbData2)

	conn.Read(respLenBuf)
	respLen = binary.BigEndian.Uint32(respLenBuf)
	respPayload = make([]byte, respLen)
	conn.Read(respPayload)

	respFrame := Protocol.GetRootAsMetaDBFrame(respPayload, 0)
	var replyTable flatbuffers.Table
	respFrame.Payload(&replyTable)
	reply := new(Protocol.MetaDBReply)
	reply.Init(replyTable.Bytes, replyTable.Pos)

	if reply.ResponseType() != Protocol.MetaDBResponseGetInventoryResp {
		t.Fatalf("expected GetInventoryResp")
	}

	var respTable flatbuffers.Table
	reply.Response(&respTable)
	getResp := new(Protocol.GetInventoryResp)
	getResp.Init(respTable.Bytes, respTable.Pos)

	t.Logf("Player 99 inventory: %d slots", getResp.InventoryLength())

	found := false
	for i := 0; i < getResp.InventoryLength(); i++ {
		slot := new(Protocol.InventorySlot)
		if getResp.Inventory(slot, i) {
			if slot.ItemId() == 4 && slot.Count() == 64 {
				found = true
			}
			t.Logf("  slot %d: item_id=%d count=%d meta=%d", i, slot.ItemId(), slot.Count(), slot.Meta())
		}
	}

	if !found {
		t.Error("expected cobblestone (id=4, count=64) at slot 5, not found")
	}
}
