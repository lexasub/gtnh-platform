// Package testutil provides TCP client helpers for GTNH Platform integration tests.
//
// Wire format (Gateway ↔ Client):
//
//	Ctrl: [4 bytes BE payload size][1 byte message type][FlatBuffer]
//	Bulk: push only (server→client)
//
// Wire format (Message Router):
//
//	[4 bytes BE payload size][1 byte message type][topics/data...]
package testutil

import (
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"time"

	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
)

// Gateway message types (mirrors GatewayMsg in gateway.h)
const (
	MsgPlayerAction               = 1
	MsgChunkSnapshot              = 2
	MsgEntitySnapshot             = 3
	MsgBlockUpdate                = 4
	MsgBlockAck                   = 5
	MsgInventoryUpdate            = 6
	MsgInventoryAction            = 7
	MsgBlockEntityUpdate          = 8
	MsgCraftRequest               = 9
	MsgCraftResponse              = 10
	MsgSetBlockAction             = 11
	MsgCompressedChunk            = 12
	MsgToolAction                 = 13
	MsgToolActionResp             = 14
	MsgSetMachineSlot             = 15
	MsgSetMachineSlotResp         = 16
	MsgMachineOpenReq             = 18
	MsgChestOpenReq               = 19
	MsgQuestProgressUpdate        = 20
	MsgQuestUnlockNotification    = 21
	MsgQuestCompletedNotification = 22
	MsgMultiblockEvent            = 23
	MsgQuestCompleteRequest       = 24
	MsgGameModeChange             = 30
	MsgStartScenarioReq           = 31
	MsgStartScenarioResp          = 32
	MsgQuestBookOpen              = 33
	MsgWorkbenchOpenReq           = 44
	MsgChestCloseReq              = 45
	MsgMachineCloseReq            = 46
	MsgResourceBufferState        = 47
	MsgPipeContentsReq            = 48
	MsgPipeContentsResp           = 49
)

// GatewayAddress holds ctrl and bulk addresses.
type GatewayAddress struct {
	CtrlHost string
	CtrlPort int
	BulkHost string
	BulkPort int
}

func DefaultGateway() GatewayAddress {
	return GatewayAddress{
		CtrlHost: "127.0.0.1", CtrlPort: 7777,
		BulkHost: "127.0.0.1", BulkPort: 7778,
	}
}

// GatewayClient is a TCP connection to the Gateway ctrl port.
type GatewayClient struct {
	conn                   net.Conn
	addr                   GatewayAddress
	pendingMultiblockEvent [][]byte
}

// DialGateway connects to the Gateway ctrl port.
func DialGateway(addr GatewayAddress, timeout time.Duration) (*GatewayClient, error) {
	target := fmt.Sprintf("%s:%d", addr.CtrlHost, addr.CtrlPort)
	conn, err := net.DialTimeout("tcp", target, timeout)
	if err != nil {
		return nil, fmt.Errorf("dial gateway %s: %w", target, err)
	}
	return &GatewayClient{conn: conn, addr: addr}, nil
}

// Close closes the connection.
func (c *GatewayClient) Close() {
	if c.conn != nil {
		c.conn.Close()
	}
}

// SendCtrl sends a message to the Gateway ctrl port.
// Wire format: [4 bytes BE payload size][1 byte msg_type][FlatBuffer data]
func (c *GatewayClient) SendCtrl(msgType uint8, fbData []byte) error {
	totalLen := 1 + len(fbData) // msg_type + FB
	if totalLen > int(^uint32(0)) {
		return fmt.Errorf("ctrl frame too large: %d", totalLen)
	}
	frame := make([]byte, 4+totalLen)
	binary.BigEndian.PutUint32(frame[0:4], uint32(totalLen))
	frame[4] = msgType
	copy(frame[5:], fbData)

	_, err := c.conn.Write(frame)
	return err
}

// ReadCtrl reads one message from the Gateway ctrl port.
// Returns (msg_type, FlatBuffer data, error).
func (c *GatewayClient) ReadCtrl(timeout time.Duration) (uint8, []byte, error) {
	if timeout > 0 {
		c.conn.SetReadDeadline(time.Now().Add(timeout))
	}

	header := make([]byte, 5)
	if _, err := io.ReadFull(c.conn, header); err != nil {
		return 0, nil, fmt.Errorf("read header: %w", err)
	}

	payloadLen := binary.BigEndian.Uint32(header[0:4])
	msgType := header[4]

	if payloadLen < 1 {
		return 0, nil, fmt.Errorf("invalid payload length: %d", payloadLen)
	}

	fbLen := payloadLen - 1
	if fbLen == 0 {
		return msgType, nil, nil
	}

	fbData := make([]byte, fbLen)
	if _, err := io.ReadFull(c.conn, fbData); err != nil {
		return 0, nil, fmt.Errorf("read fb data: %w", err)
	}

	return msgType, fbData, nil
}

// ExpectQuestCompletion waits for both completion and progress notifications
// and returns their raw payloads. Unrelated push frames are consumed because
// the connection is dedicated to this tracer.
func (c *GatewayClient) ExpectQuestCompletion(playerID uint64, questID uint32, timeout time.Duration) (completed, progress []byte, err error) {
	deadline := time.Now().Add(timeout)
	for completed == nil || progress == nil {
		remaining := time.Until(deadline)
		if remaining <= 0 {
			return completed, progress, fmt.Errorf("timeout waiting for quest completion player=%d quest=%d (completed=%t progress=%t)", playerID, questID, completed != nil, progress != nil)
		}
		msgType, data, readErr := c.ReadCtrl(remaining)
		if readErr != nil {
			return completed, progress, fmt.Errorf(
				"trace quest completion player=%d quest=%d completed=%t progress=%t: %w",
				playerID, questID, completed != nil, progress != nil, readErr)
		}
		switch msgType {
		case MsgQuestCompletedNotification:
			notification := Protocol.GetRootAsQuestCompletedNotification(data, 0)
			if notification.PlayerId() == playerID && notification.QuestId() == questID {
				completed = data
			}
		case MsgQuestProgressUpdate:
			update := Protocol.GetRootAsQuestProgressUpdate(data, 0)
			if update.PlayerId() != playerID {
				continue
			}
			for i := 0; i < update.QuestsLength(); i++ {
				entry := Protocol.QuestEntry{}
				if update.Quests(&entry, i) && entry.QuestId() == questID && entry.Status() == Protocol.QuestStatusCOMPLETED && entry.Progress() == 100 {
					progress = data
				}
			}
		}
	}
	return completed, progress, nil
}

// ExpectMsgType reads and verifies the message type is the expected one.
// Skips unexpected message types (push notifications from gateway) in a loop.
func (c *GatewayClient) ExpectMsgType(expected uint8, timeout time.Duration) ([]byte, error) {
	if expected == MsgMultiblockEvent && len(c.pendingMultiblockEvent) > 0 {
		data := c.pendingMultiblockEvent[0]
		c.pendingMultiblockEvent = c.pendingMultiblockEvent[1:]
		return data, nil
	}
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		remaining := time.Until(deadline)
		if remaining < 10*time.Millisecond {
			remaining = 10 * time.Millisecond
		}
		msgType, data, err := c.ReadCtrl(remaining)
		if err != nil {
			return nil, err
		}
		if msgType == expected {
			return data, nil
		}
		// Unexpected type — skip and retry (push notifications are interleaved)
	}
	return nil, fmt.Errorf("timeout waiting for msg_type %d", expected)
}

// WaitForInventoryItem waits for an InventoryUpdate snapshot belonging to
// playerID whose slots hold itemID with a total count of at least minCount.
// Gateway forwards queued player.inventory.update pushes without correlation,
// so unmatched snapshots (empty payload, other player, other item, smaller
// total) are skipped until the deadline.
//
// The count is summed across every matching slot because the server splits
// stacks across slots.
func (c *GatewayClient) WaitForInventoryItem(playerID uint64, itemID uint16, minCount byte, timeout time.Duration) ([]byte, error) {
	deadline := time.Now().Add(timeout)
	for {
		remaining := time.Until(deadline)
		if remaining <= 0 {
			return nil, fmt.Errorf("timeout waiting for inventory player=%d item=%d total_count>=%d", playerID, itemID, minCount)
		}
		if remaining < 10*time.Millisecond {
			remaining = 10 * time.Millisecond
		}
		msgType, data, err := c.ReadCtrl(remaining)
		if err != nil {
			// A read that expires exactly at the deadline is the timeout path,
			// not a transport failure; report the predicate that never matched.
			if time.Now().Before(deadline) {
				return nil, fmt.Errorf("wait for inventory player=%d item=%d total_count>=%d: %w", playerID, itemID, minCount, err)
			}
			return nil, fmt.Errorf("timeout waiting for inventory player=%d item=%d total_count>=%d", playerID, itemID, minCount)
		}
		// A zero-length FlatBuffer has no root table; GetRootAs* would panic.
		if msgType != MsgInventoryUpdate || len(data) == 0 {
			continue
		}
		update := Protocol.GetRootAsInventoryUpdate(data, 0)
		if update.PlayerId() != playerID {
			continue
		}
		total := 0
		var slot Protocol.InventorySlot
		for i := 0; i < update.SlotsLength(); i++ {
			if !update.Slots(&slot, i) {
				continue
			}
			if slot.ItemId() == itemID {
				total += int(slot.Count())
			}
		}
		if total >= int(minCount) {
			return data, nil
		}
	}
}

// InventoryItemCount sums the counts of itemID across every player slot in an
// InventoryUpdate payload. The server splits stacks across slots
// (PlayerInventoryStore stacking, max 64), so a per-slot check is not enough.
// Returns 0 for a payload with no root table.
func InventoryItemCount(data []byte, itemID uint16) int {
	if len(data) == 0 {
		return 0
	}
	update := Protocol.GetRootAsInventoryUpdate(data, 0)
	if update == nil {
		return 0
	}
	total := 0
	var slot Protocol.InventorySlot
	for i := 0; i < update.SlotsLength(); i++ {
		if update.Slots(&slot, i) && slot.ItemId() == itemID {
			total += int(slot.Count())
		}
	}
	return total
}

// FirstSlotWithItem returns the index of the first player slot in an
// InventoryUpdate payload holding itemID, or -1 when the item is not in the
// player grid. Item grants land in the first free slot, which is not
// necessarily 0, so a click fixture must locate the real slot.
func FirstSlotWithItem(data []byte, itemID uint16) int {
	if len(data) == 0 {
		return -1
	}
	update := Protocol.GetRootAsInventoryUpdate(data, 0)
	if update == nil {
		return -1
	}
	var slot Protocol.InventorySlot
	for i := 0; i < update.SlotsLength(); i++ {
		if update.Slots(&slot, i) && slot.ItemId() == itemID {
			return i
		}
	}
	return -1
}

// WaitForBlockAck waits for the ACK matching both request ID and status.
// Gateway pushes may be interleaved with ACKs, so unrelated complete frames
// are consumed and ignored. The ACCEPTED status is the placement protocol's
// immediate client acknowledgement; the asynchronous CAS result is observed
// separately by the server and must not add a five-second delay to each step.
func (c *GatewayClient) WaitForBlockAck(requestID uint32, status Protocol.BlockAckStatus, timeout time.Duration) ([]byte, error) {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		remaining := time.Until(deadline)
		if remaining < 10*time.Millisecond {
			remaining = 10 * time.Millisecond
		}
		msgType, data, err := c.ReadCtrl(remaining)
		if err != nil {
			return nil, err
		}
		if msgType == MsgMultiblockEvent {
			c.pendingMultiblockEvent = append(c.pendingMultiblockEvent, data)
			continue
		}
		if msgType != MsgBlockAck {
			continue
		}
		ack := Protocol.GetRootAsBlockAck(data, 0)
		if ack != nil && ack.RequestId() == requestID && ack.Status() == status {
			return data, nil
		}
	}
	return nil, fmt.Errorf("timeout waiting for BlockAck request_id=%d status=%v", requestID, status)
}

// RequestChunk asks Gateway/ChunkStore to generate and cache a chunk before
// a CAS placement touches it. This removes the generation-vs-CAS startup race.
func (c *GatewayClient) RequestChunk(playerID uint64, chunkX, chunkY, chunkZ int32) error {
	return c.SendCtrl(MsgPlayerAction, BuildPlayerAction(playerID,
		Protocol.PlayerActionTypeCHUNK_REQUEST, chunkX, chunkY, chunkZ, 0, 0))
}

// WaitForChunkGeneration gives the asynchronous world generator time to
// persist a requested chunk. The chunk protocol has no request ACK.
func (c *GatewayClient) WaitForChunkGeneration(timeout time.Duration) {
	time.Sleep(timeout)
}

// MultiblockEventKind identifies the typed payload carried by message 23.
type MultiblockEventKind uint8

const (
	MultiblockEventUnknown   MultiblockEventKind = 0
	MultiblockEventCreated   MultiblockEventKind = 1
	MultiblockEventDestroyed MultiblockEventKind = 2
)

// DecodeMultiblockEvent decodes a type-23 payload as a created or destroyed event.
func DecodeMultiblockEvent(data []byte) (MultiblockEventKind, *Protocol.MultiblockCreatedEvent, *Protocol.MultiblockDestroyedEvent, error) {
	if len(data) == 0 {
		return MultiblockEventUnknown, nil, nil, fmt.Errorf("empty multiblock event")
	}
	created := Protocol.GetRootAsMultiblockCreatedEvent(data, 0)
	var anchor Protocol.Vec3i
	if created != nil && created.Anchor(&anchor) != nil {
		return MultiblockEventCreated, created, nil, nil
	}
	destroyed := Protocol.GetRootAsMultiblockDestroyedEvent(data, 0)
	if destroyed != nil {
		return MultiblockEventDestroyed, nil, destroyed, nil
	}
	return MultiblockEventUnknown, nil, nil, fmt.Errorf("unknown multiblock event payload")
}

// ExpectMultiblockEvent waits for a typed message-23 lifecycle event.
func (c *GatewayClient) ExpectMultiblockEvent(timeout time.Duration) (MultiblockEventKind, *Protocol.MultiblockCreatedEvent, *Protocol.MultiblockDestroyedEvent, error) {
	if len(c.pendingMultiblockEvent) > 0 {
		data := c.pendingMultiblockEvent[0]
		c.pendingMultiblockEvent = c.pendingMultiblockEvent[1:]
		return DecodeMultiblockEvent(data)
	}
	data, err := c.ExpectMsgType(MsgMultiblockEvent, timeout)
	if err != nil {
		return MultiblockEventUnknown, nil, nil, err
	}
	return DecodeMultiblockEvent(data)
}

// DrainUnexpected reads and discards all pending messages up to timeout.
// Useful between test steps to clear push notifications from the buffer.
func (c *GatewayClient) DrainUnexpected(timeout time.Duration) {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		remaining := time.Until(deadline)
		if remaining < 10*time.Millisecond {
			break
		}
		_, _, err := c.ReadCtrl(remaining)
		if err != nil {
			return // timeout or connection closed — buffer is empty
		}
	}
}

// ============================================================================
// RouterClient — publish/subscribe directly to MessageRouter
// ============================================================================

// RouterClient connects to the MessageRouter for pub/sub.
type RouterClient struct {
	conn net.Conn
}

// DialRouter connects to the MessageRouter.
func DialRouter(host string, port int, timeout time.Duration) (*RouterClient, error) {
	target := fmt.Sprintf("%s:%d", host, port)
	conn, err := net.DialTimeout("tcp", target, timeout)
	if err != nil {
		return nil, fmt.Errorf("dial router %s: %w", target, err)
	}
	return &RouterClient{conn: conn}, nil
}

// Close closes the connection.
func (r *RouterClient) Close() {
	if r.conn != nil {
		r.conn.Close()
	}
}

// Conn exposes the underlying TCP connection for direct I/O.
func (r *RouterClient) Conn() net.Conn {
	return r.conn
}

// WriteFrame sends a raw FlatBuffer to a service that uses the
// [4 bytes BE length][FlatBuffer] wire format (ChunkStore, EntityStateStore).
func WriteFrame(conn net.Conn, fbData []byte) error {
	frame := make([]byte, 4+len(fbData))
	binary.BigEndian.PutUint32(frame[0:4], uint32(len(fbData)))
	copy(frame[4:], fbData)
	_, err := conn.Write(frame)
	return err
}

// ReadFrameRaw reads a [4 bytes BE length][payload] frame from a connection.
func ReadFrameRaw(conn net.Conn, timeout time.Duration) ([]byte, error) {
	if timeout > 0 {
		conn.SetReadDeadline(time.Now().Add(timeout))
	}
	lenBuf := make([]byte, 4)
	if _, err := io.ReadFull(conn, lenBuf); err != nil {
		return nil, fmt.Errorf("read frame length: %w", err)
	}
	payloadLen := binary.BigEndian.Uint32(lenBuf)
	if payloadLen == 0 {
		return nil, nil
	}
	payload := make([]byte, payloadLen)
	if _, err := io.ReadFull(conn, payload); err != nil {
		return nil, fmt.Errorf("read frame payload: %w", err)
	}
	return payload, nil
}

// Router wire protocol frame types (from message_router)
const (
	RouterMsgSubscribe   = 1
	RouterMsgUnsubscribe = 2
	RouterMsgPublish     = 3
	RouterMsgRegister    = 4
	RouterMsgHeartbeat   = 5
)

// Subscribe sends a subscribe frame to the router.
func (r *RouterClient) Subscribe(topic string) error {
	return r.sendStringFrame(RouterMsgSubscribe, topic)
}

// Publish sends data on a topic.
func (r *RouterClient) Publish(topic string, data []byte) error {
	// Frame: [msg_type=3][topic_len(2)][topic][data]
	payload := make([]byte, 2+len(topic)+len(data))
	binary.BigEndian.PutUint16(payload[0:2], uint16(len(topic)))
	copy(payload[2:], topic)
	copy(payload[2+len(topic):], data)

	frame := make([]byte, 4+len(payload))
	binary.BigEndian.PutUint32(frame[0:4], uint32(len(payload)))
	frame[4] = RouterMsgPublish
	copy(frame[5:], payload)

	_, err := r.conn.Write(frame)
	return err
}

// ReadFrame reads one router frame. Returns (msg_type, payload, error).
func (r *RouterClient) ReadFrame(timeout time.Duration) (uint8, []byte, error) {
	if timeout > 0 {
		r.conn.SetReadDeadline(time.Now().Add(timeout))
	}
	header := make([]byte, 5)
	if _, err := io.ReadFull(r.conn, header); err != nil {
		return 0, nil, fmt.Errorf("read frame header: %w", err)
	}
	payloadLen := binary.BigEndian.Uint32(header[0:4])
	msgType := header[4]
	payload := make([]byte, payloadLen)
	if payloadLen > 0 {
		if _, err := io.ReadFull(r.conn, payload); err != nil {
			return 0, nil, fmt.Errorf("read frame payload: %w", err)
		}
	}
	return msgType, payload, nil
}

// readTopicPayload reads topic + data from a publish frame payload.
// Returns (topic, data).
func ReadTopicPayload(payload []byte) (string, []byte, error) {
	if len(payload) < 2 {
		return "", nil, fmt.Errorf("payload too short")
	}
	topicLen := binary.BigEndian.Uint16(payload[0:2])
	if int(2+topicLen) > len(payload) {
		return "", nil, fmt.Errorf("topic length %d exceeds payload", topicLen)
	}
	topic := string(payload[2 : 2+topicLen])
	data := payload[2+topicLen:]
	return topic, data, nil
}

func (r *RouterClient) sendStringFrame(msgType uint8, s string) error {
	payload := make([]byte, 2+len(s)+1) // 2 = string len
	binary.BigEndian.PutUint16(payload[0:2], uint16(len(s)))
	copy(payload[2:], s)

	frame := make([]byte, 4+len(payload))
	binary.BigEndian.PutUint32(frame[0:4], uint32(len(payload)))
	frame[4] = msgType
	copy(frame[5:], payload)

	_, err := r.conn.Write(frame)
	return err
}
