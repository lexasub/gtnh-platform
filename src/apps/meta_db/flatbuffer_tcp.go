package main

import (
	"encoding/binary"
	"io"
	"log"
	"net"

	flatbuffers "github.com/google/flatbuffers/go"

	"github.com/gtnh-platform/protocol/generated/go/Protocol"
)

const fbPort = ":5006"

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

func handleFlatBufferConnection(conn net.Conn, m *MetaDB) {
	defer conn.Close()

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

		payload := make([]byte, payloadLen)
		if _, err := io.ReadFull(conn, payload); err != nil {
			log.Printf("FB read payload error: %v", err)
			return
		}

		frame := Protocol.GetRootAsMetaDBFrame(payload, 0)

		respData := dispatchFlatBufferFrame(frame, m)
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
