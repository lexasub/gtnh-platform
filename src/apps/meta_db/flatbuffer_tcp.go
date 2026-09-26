package main

import (
	"encoding/binary"
	"io"
	"log"
	"net"
	"runtime/debug"

	flatbuffers "github.com/google/flatbuffers/go"

	"github.com/gtnh-platform/protocol/generated/go/Protocol"
)

const fbPort = ":5006"

// maxFBFrameBytes caps the allocation a single 4-byte length prefix can
// request. gp-3v5x: the length is remote input; unchecked, one frame header can
// ask for a 4 GiB allocation and abort the process.
const maxFBFrameBytes = 16 << 20

// debugStack renders the stack of a recovered panic for the log. A recover()
// that swallows the stack is how the original defect stayed invisible.
func debugStack() string {
	return string(debug.Stack())
}

func startFlatBufferListener(m *MetaDB) {
	listener, err := net.Listen("tcp", fbPort)
	if err != nil {
		log.Fatalf("Failed to listen on FlatBuffers port %s: %v", fbPort, err)
	}
	defer listener.Close()

	log.Printf("MetaDB FlatBuffers handler listening on %s", fbPort)

	for {
		conn, err := listener.Accept()
		if err != nil {
			log.Printf("FlatBuffers accept error: %v", err)
			continue
		}
		go handleFlatBufferConnection(conn, m)
	}
}

// fbFrameDispatch is the frame-handling seam: it turns one verified frame into
// the reply bytes, or nil for a fire-and-forget frame. It is a named function
// value so the recover() backstop below can be exercised directly by a test (a
// structurally valid frame that panics in dispatch) — the verifier makes that
// unreachable in production, which is exactly why it needs a seam to test.
type fbFrameDispatch func(frame *Protocol.MetaDBFrame, m *MetaDB) []byte

// handleFlatBufferConnection serves one client connection with the production
// frame dispatcher. See handleFlatBufferConnectionWith for the full rationale.
func handleFlatBufferConnection(conn net.Conn, m *MetaDB) {
	handleFlatBufferConnectionWith(conn, m, dispatchFlatBufferFrame)
}

// handleFlatBufferConnectionWith serves one client connection, dispatching
// each verified frame through the given handler.
//
// gp-3v5x: this goroutine parses attacker-controlled frames and had no recover(),
// so a single malformed frame panicked in flatbuffers/go's Table.Offset
// (GetSOffsetT reads a 4-byte int at an unvalidated position — the audit's live
// probe produced "index out of range [3] with length 0") and took the whole
// metadbd process down. One bad frame was a remote DoS.
//
// Two layers, mirroring the C++ fix that closed the same hole
// (src/game/storage/InventoryLoadHandler.cpp:21-26,
// src/game/storage/PlayerJoinedHandler.cpp:22-27):
//
//  1. verifyRoot rejects the malformed buffer BEFORE any accessor runs, so the
//     frame is dropped and the connection continues. This is the primary
//     defence and the one that does the work day to day.
//  2. recover() is the backstop for anything the verifier did not model — a new
//     table, a new accessor, a nil deref in a handler. Go's flatbuffers package
//     ships no Verifier, so this verifier is necessarily hand-written and will
//     need to be extended when a table is added; without the backstop, that
//     extension gap is a process kill rather than a dropped frame.
//
// DECISION — close the connection, do not continue reading. Two distinct
// paths, two different answers:
//
//  1. VERIFIER REJECTION (the ordinary case): the frame is dropped and the
//     loop continues. Nothing was parsed, nothing was written, and the length
//     prefix of the next frame is still the next 4 bytes on the wire, so the
//     stream is perfectly in sync. A client that sends one bad frame keeps its
//     connection. This is the same R1/R2 "drop and return" shape as the C++
//     handlers, minus the process-kill.
//
//  2. RECOVERED PANIC (the backstop): the connection is CLOSED. A panic means
//     the handler was interrupted at an unknown point and may have written a
//     partial response, so the byte stream is no longer trustworthy — the next
//     4 bytes could be the tail of a half-written response rather than a length
//     prefix. Resynchronising a length-prefixed stream at an unknown position
//     is not something that can be done safely, and continuing would risk
//     acting on a misparsed player id. Closing is the only sound recovery for a
//     stateful byte stream; the client reconnects. This is cheap here because
//     the topics MetaDB serves are fire-and-forget events, not a long-lived
//     session, and because path 1 already absorbs every malformed frame the
//     verifier models — so a recovered panic means either a new table the
//     verifier does not cover yet, or a bug in a handler, both of which deserve
//     a loud drop rather than a silent retry loop.
func handleFlatBufferConnectionWith(conn net.Conn, m *MetaDB, dispatch fbFrameDispatch) {
	defer conn.Close()

	// Backstop for anything the verifier does not model. Recovering here
	// confines the damage to this connection instead of the process. Note the
	// return: the deferred conn.Close() above then tears the connection down,
	// which is the close-don't-continue half of the decision above.
	defer func() {
		if r := recover(); r != nil {
			log.Printf("FB: panic while serving connection — dropping connection: %v\n%s",
				r, debugStack())
		}
	}()

	for {
		lenBuf := make([]byte, 4)
		if _, err := io.ReadFull(conn, lenBuf); err != nil {
			if err != io.EOF {
				log.Printf("FB read length error: %v", err)
			}
			return
		}
		payloadLen := binary.BigEndian.Uint32(lenBuf)

		if payloadLen == 0 {
			continue
		}

		// gp-3v5x: a 4-byte length prefix is remote input, so cap the
		// allocation. Without this a single frame header can request a 4 GiB
		// allocation and abort the process outright.
		if payloadLen > maxFBFrameBytes {
			log.Printf("FB: refusing oversized frame of %d bytes (max %d) — dropping connection", payloadLen, maxFBFrameBytes)
			return
		}

		payload := make([]byte, payloadLen)
		if _, err := io.ReadFull(conn, payload); err != nil {
			log.Printf("FB read payload error: %v", err)
			return
		}

		// Layer 1: structural validation before the buffer is ever handed to a
		// generated accessor. GetRootAsMetaDBFrame never returns nil, so this
		// replaces the dead "if frame == nil" check with one that can actually
		// fire.
		frame, err := fbVerifyFrame(payload)
		if err != nil {
			log.Printf("FB: dropping malformed frame (%d bytes): %v", len(payload), err)
			continue
		}

		respData := dispatch(frame, m)
		if respData == nil {
			continue
		}

		respLenBuf := make([]byte, 4)
		binary.BigEndian.PutUint32(respLenBuf, uint32(len(respData)))
		if _, err := conn.Write(respLenBuf); err != nil {
			log.Printf("FB write length error: %v", err)
			return
		}
		if _, err := conn.Write(respData); err != nil {
			log.Printf("FB write payload error: %v", err)
			return
		}
	}
}

func dispatchFlatBufferFrame(frame *Protocol.MetaDBFrame, m *MetaDB) []byte {
	payloadType := frame.PayloadType()
	switch payloadType {
	case Protocol.MetaDBPayloadMetaDBMessage:
		var msgTable flatbuffers.Table
		if !frame.Payload(&msgTable) {
			log.Printf("FB: failed to extract Message from frame")
			return nil
		}
		msg := new(Protocol.MetaDBMessage)
		msg.Init(msgTable.Bytes, msgTable.Pos)
		return handleMetaDBMessage(msg, m)

	case Protocol.MetaDBPayloadMetaDBReply:
		log.Printf("FB: received unexpected reply frame")
		return nil

	default:
		log.Printf("FB: unknown payload type: %v", payloadType)
		return nil
	}
}

func handleMetaDBMessage(msg *Protocol.MetaDBMessage, m *MetaDB) []byte {
	reqID := msg.ReqId()
	reqType := msg.RequestType()

	// gp-3v5x: the per-request accessors below used to be reached via a bare
	// req.Init(reqTable.Bytes, reqTable.Pos) with no structural check, so a
	// malformed union payload sliced out of range. Verify the concrete request
	// table against its .fbs descriptor before any accessor runs. The
	// MsgTable-shaped !msg.Request(&reqTable) checks below are kept as-is: they
	// are real (Request returns false when the offset is absent) and are part
	// of the existing ACCEPTED/status ordering.
	if desc := fbRequestDescriptor(reqType); desc != nil {
		var reqTab flatbuffers.Table
		if msg.Request(&reqTab) {
			if err := verifyTableAt(reqTab.Bytes, reqTab.Pos, desc, 1); err != nil {
				log.Printf("FB: dropping malformed %v request: %v", reqType, err)
				return buildErrorResp(reqID, "malformed request")
			}
		}
	}

	var reqTable flatbuffers.Table

	switch reqType {
	case Protocol.MetaDBRequestGetInventoryReq:
		if !msg.Request(&reqTable) {
			log.Printf("FB: failed to extract GetInventoryReq")
			return buildErrorResp(reqID, "failed to parse GetInventoryReq")
		}
		req := new(Protocol.GetInventoryReq)
		req.Init(reqTable.Bytes, reqTable.Pos)
		return handleGetInventoryReq(reqID, req, m)

	case Protocol.MetaDBRequestSetInventorySlotReq:
		if !msg.Request(&reqTable) {
			log.Printf("FB: failed to extract SetInventorySlotReq")
			return buildErrorResp(reqID, "failed to parse SetInventorySlotReq")
		}
		req := new(Protocol.SetInventorySlotReq)
		req.Init(reqTable.Bytes, reqTable.Pos)
		return handleSetInventorySlotReq(reqID, req, m)

	case Protocol.MetaDBRequestGetInventorySnapshotReq:
		if !msg.Request(&reqTable) {
			log.Printf("FB: failed to extract GetInventorySnapshotReq")
			return buildErrorResp(reqID, "failed to parse GetInventorySnapshotReq")
		}
		req := new(Protocol.GetInventorySnapshotReq)
		req.Init(reqTable.Bytes, reqTable.Pos)
		return handleGetInventorySnapshotReq(reqID, req, m)

	default:
		log.Printf("FB: unknown request type: %v", reqType)
		return buildErrorResp(reqID, "unknown request type")
	}
}

func handleGetInventoryReq(reqID uint32, req *Protocol.GetInventoryReq, m *MetaDB) []byte {
	playerID := req.PlayerId()
	slots, err := m.GetInventory(playerID)
	if err != nil {
		return buildErrorResp(reqID, "GetInventory failed: "+err.Error())
	}

	m.PublishInventoryTo("player.inventory.load", playerID, slots)

	builder := flatbuffers.NewBuilder(1024)

	slotOffsets := make([]flatbuffers.UOffsetT, len(slots))
	for i, s := range slots {
		Protocol.InventorySlotStart(builder)
		Protocol.InventorySlotAddItemId(builder, s.BlockID)
		Protocol.InventorySlotAddCount(builder, s.Count)
		Protocol.InventorySlotAddMeta(builder, s.Meta)
		slotOffsets[i] = Protocol.InventorySlotEnd(builder)
	}

	Protocol.GetInventoryRespStartInventoryVector(builder, len(slotOffsets))
	for i := len(slotOffsets) - 1; i >= 0; i-- {
		builder.PrependUOffsetT(slotOffsets[i])
	}
	inventoryVec := builder.EndVector(len(slotOffsets))

	Protocol.GetInventoryRespStart(builder)
	Protocol.GetInventoryRespAddInventory(builder, inventoryVec)
	respOffset := Protocol.GetInventoryRespEnd(builder)

	return buildReplyFrame(builder, reqID, Protocol.MetaDBResponseGetInventoryResp, respOffset)
}

func handleSetInventorySlotReq(reqID uint32, req *Protocol.SetInventorySlotReq, m *MetaDB) []byte {
	playerID := req.PlayerId()
	slotIndex := int(req.SlotIndex())
	itemID := int(req.ItemId())
	count := int(req.Count())
	log.Printf("[FB] handleSetInventorySlotReq: player=%d slot=%d item=%d count=%d", playerID, slotIndex, itemID, count)

	err := m.UpdateInventorySlot(playerID, slotIndex, itemID, count)
	if err != nil {
		return buildErrorResp(reqID, "UpdateInventorySlot failed: "+err.Error())
	}

	// empty GetInventoryResp signals success (no dedicated SetInventorySlotResp in schema)
	builder := flatbuffers.NewBuilder(64)
    invVec := builder.CreateByteVector(nil)
	Protocol.GetInventoryRespStart(builder)
	Protocol.GetInventoryRespAddInventory(builder, invVec)
	respOffset := Protocol.GetInventoryRespEnd(builder)

	return buildReplyFrame(builder, reqID, Protocol.MetaDBResponseGetInventoryResp, respOffset)
}

func handleGetInventorySnapshotReq(reqID uint32, req *Protocol.GetInventorySnapshotReq, m *MetaDB) []byte {
	playerID := req.PlayerId()
	slots, err := m.GetInventory(playerID)
	if err != nil {
		return buildErrorResp(reqID, "GetInventory failed: "+err.Error())
	}

	builder := flatbuffers.NewBuilder(1024)

	mainOffsets := make([]flatbuffers.UOffsetT, len(slots))
	for i, s := range slots {
		Protocol.InventorySlotStart(builder)
		Protocol.InventorySlotAddItemId(builder, s.BlockID)
		Protocol.InventorySlotAddCount(builder, s.Count)
		Protocol.InventorySlotAddMeta(builder, s.Meta)
		mainOffsets[i] = Protocol.InventorySlotEnd(builder)
	}

	Protocol.GetInventorySnapshotRespStartMainInventoryVector(builder, len(mainOffsets))
	for i := len(mainOffsets) - 1; i >= 0; i-- {
		builder.PrependUOffsetT(mainOffsets[i])
	}
	mainVec := builder.EndVector(len(mainOffsets))

	Protocol.GetInventorySnapshotRespStartHotbarVector(builder, 0)
	hotbarVec := builder.EndVector(0)

	Protocol.GetInventorySnapshotRespStart(builder)
	Protocol.GetInventorySnapshotRespAddMainInventory(builder, mainVec)
	Protocol.GetInventorySnapshotRespAddHotbar(builder, hotbarVec)
	respOffset := Protocol.GetInventorySnapshotRespEnd(builder)

	return buildReplyFrame(builder, reqID, Protocol.MetaDBResponseGetInventorySnapshotResp, respOffset)
}

func buildReplyFrame(builder *flatbuffers.Builder, reqID uint32, respType Protocol.MetaDBResponse, respOffset flatbuffers.UOffsetT) []byte {
	Protocol.MetaDBReplyStart(builder)
	Protocol.MetaDBReplyAddReqId(builder, reqID)
	Protocol.MetaDBReplyAddResponseType(builder, respType)
	Protocol.MetaDBReplyAddResponse(builder, respOffset)
	replyOffset := Protocol.MetaDBReplyEnd(builder)

	Protocol.MetaDBFrameStart(builder)
	Protocol.MetaDBFrameAddPayloadType(builder, Protocol.MetaDBPayloadMetaDBReply)
	Protocol.MetaDBFrameAddPayload(builder, replyOffset)
	frameOffset := Protocol.MetaDBFrameEnd(builder)

	builder.Finish(frameOffset)
	return builder.FinishedBytes()
}

func buildErrorResp(reqID uint32, message string) []byte {
	builder := flatbuffers.NewBuilder(256)
	msgOffset := builder.CreateString(message)

	Protocol.ErrorRespStart(builder)
	Protocol.ErrorRespAddMessage(builder, msgOffset)
	errOffset := Protocol.ErrorRespEnd(builder)

	return buildReplyFrame(builder, reqID, Protocol.MetaDBResponseErrorResp, errOffset)
}