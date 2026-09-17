package testutil

import (
	"encoding/binary"
	"fmt"
	"net"
	"sync/atomic"
	"time"

	flatbuffers "github.com/google/flatbuffers/go"
	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
)

// ChunkStoreClient reads authoritative block state over ChunkStore's direct RPC
// connection. Gateway's ACCEPTED BlockAck only confirms CAS submission; this
// client provides the commit boundary needed to pace a placement fixture.
type ChunkStoreClient struct {
	conn   net.Conn
	nextID atomic.Uint32
}

func DialChunkStore(host string, port int, timeout time.Duration) (*ChunkStoreClient, error) {
	target := fmt.Sprintf("%s:%d", host, port)
	conn, err := net.DialTimeout("tcp", target, timeout)
	if err != nil {
		return nil, fmt.Errorf("dial ChunkStore %s: %w", target, err)
	}
	client := &ChunkStoreClient{conn: conn}
	client.nextID.Store(1)
	return client, nil
}

func (c *ChunkStoreClient) Close() {
	if c.conn != nil {
		_ = c.conn.Close()
	}
}

// WriteChunkStoreFrame sends the transport frame expected by the direct
// io_uring ChunkStore service: [4-byte BE length][type byte][FlatBuffer].
func WriteChunkStoreFrame(conn net.Conn, fbData []byte) error {
	frame := make([]byte, 5+len(fbData))
	binary.BigEndian.PutUint32(frame[:4], uint32(len(fbData)+1))
	frame[4] = 0
	copy(frame[5:], fbData)
	_, err := conn.Write(frame)
	return err
}

func ReadChunkStoreFrame(conn net.Conn, timeout time.Duration) ([]byte, error) {
	return ReadFrameRaw(conn, timeout)
}

func (c *ChunkStoreClient) GetBlock(x, y, z int32, timeout time.Duration) (uint16, uint8, uint32, error) {
	builder := flatbuffers.NewBuilder(64)
	Protocol.GetBlockReqStart(builder)
	pos := Protocol.CreateVec3i(builder, x, y, z)
	Protocol.GetBlockReqAddPos(builder, pos)
	req := Protocol.GetBlockReqEnd(builder)

	Protocol.ChunkStoreMessageStart(builder)
	Protocol.ChunkStoreMessageAddReqId(builder, c.nextID.Add(1)-1)
	Protocol.ChunkStoreMessageAddRequestType(builder, Protocol.ChunkStoreRequestGetBlockReq)
	Protocol.ChunkStoreMessageAddRequest(builder, req)
	message := Protocol.ChunkStoreMessageEnd(builder)

	Protocol.ChunkStoreFrameStart(builder)
	Protocol.ChunkStoreFrameAddPayloadType(builder, Protocol.ChunkStorePayloadChunkStoreMessage)
	Protocol.ChunkStoreFrameAddPayload(builder, message)
	frame := Protocol.ChunkStoreFrameEnd(builder)
	builder.Finish(frame)

	if err := WriteChunkStoreFrame(c.conn, builder.FinishedBytes()); err != nil {
		return 0, 0, 0, fmt.Errorf("get block write: %w", err)
	}
	payload, err := ReadFrameRaw(c.conn, timeout)
	if err != nil {
		return 0, 0, 0, fmt.Errorf("get block read: %w", err)
	}
	if len(payload) < 1 {
		return 0, 0, 0, fmt.Errorf("get block response too short: %d", len(payload))
	}
	if payload[0] != 0 {
		return 0, 0, 0, fmt.Errorf("unexpected ChunkStore transport type: %d", payload[0])
	}
	if len(payload) < 2 {
		return 0, 0, 0, fmt.Errorf("get block FlatBuffer too short")
	}
	frameResp := Protocol.GetRootAsChunkStoreFrame(payload[1:], 0)
	if frameResp == nil || frameResp.PayloadType() != Protocol.ChunkStorePayloadChunkStoreReply {
		return 0, 0, 0, fmt.Errorf("unexpected ChunkStore response type")
	}
	var replyTable flatbuffers.Table
	if !frameResp.Payload(&replyTable) {
		return 0, 0, 0, fmt.Errorf("ChunkStore response has no reply")
	}
	reply := new(Protocol.ChunkStoreReply)
	reply.Init(replyTable.Bytes, replyTable.Pos)
	if reply.ResponseType() != Protocol.ChunkStoreResponseGetBlockResp {
		return 0, 0, 0, fmt.Errorf("unexpected ChunkStore response: %v", reply.ResponseType())
	}
	var responseTable flatbuffers.Table
	if !reply.Response(&responseTable) {
		return 0, 0, 0, fmt.Errorf("ChunkStore response has no block")
	}
	response := new(Protocol.GetBlockResp)
	response.Init(responseTable.Bytes, responseTable.Pos)
	return response.BlockId(), response.Meta(), response.MbId(), nil
}

func (c *ChunkStoreClient) WaitForBlock(x, y, z int32, expected uint16, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	var last uint16
	for time.Now().Before(deadline) {
		id, _, _, err := c.GetBlock(x, y, z, time.Until(deadline))
		if err != nil {
			return err
		}
		last = id
		if id == expected {
			return nil
		}
		time.Sleep(50 * time.Millisecond)
	}
	return fmt.Errorf("timeout waiting for authoritative block (%d,%d,%d): got %d, want %d",
		x, y, z, last, expected)
}

// PlaceBlockAndWait combines the immediate Gateway ACK with the authoritative
// ChunkStore commit check. If a CAS completion races the fixture, it retries
// the same placement with a fresh request ID until the overall deadline.
func (c *GatewayClient) PlaceBlockAndWait(cs *ChunkStoreClient, playerID uint64,
	x, y, z int32, blockID uint16, requestID uint32, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if err := c.SendCtrl(MsgSetBlockAction,
			BuildSetBlockActionWithOptions(playerID, x, y+1, z, 0, blockID,
				SetBlockActionOptions{RequestID: requestID, Face: 0, HeldItem: blockID})); err != nil {
			return err
		}
		if _, err := c.WaitForBlockAck(requestID, Protocol.BlockAckStatusACCEPTED,
			minDuration(2*time.Second, time.Until(deadline))); err != nil {
			return err
		}
		if err := cs.WaitForBlock(x, y, z, blockID,
			minDuration(500*time.Millisecond, time.Until(deadline))); err == nil {
			return nil
		}
		requestID++
	}
	return fmt.Errorf("timeout placing block %d at (%d,%d,%d)", blockID, x, y, z)
}

func minDuration(a, b time.Duration) time.Duration {
	if b < a {
		return b
	}
	return a
}
