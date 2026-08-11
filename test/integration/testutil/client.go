// Package testutil provides TCP client helpers for GTNH Platform integration tests.
//
// Wire format (Gateway ↔ Client):
//   Ctrl: [4 bytes BE payload size][1 byte message type][FlatBuffer]
//   Bulk: push only (server→client)
//
// Wire format (Message Router):
//   [4 bytes BE payload size][1 byte message type][topics/data...]
package testutil

import (
	"encoding/binary"
	"fmt"
	"net"
	"time"
)

// Gateway message types (mirrors GatewayMsg in gateway.h)
const (
	MsgPlayerAction      = 1
	MsgChunkSnapshot     = 2
	MsgEntitySnapshot    = 3
	MsgBlockUpdate       = 4
	MsgBlockAck          = 5
	MsgInventoryUpdate   = 6
	MsgInventoryAction   = 7
	MsgBlockEntityUpdate = 8
	MsgCraftRequest      = 9
	MsgCraftResponse     = 10
	MsgSetBlockAction    = 11
	MsgCompressedChunk   = 12
	MsgSetMachineSlot    = 15
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
	conn net.Conn
	addr GatewayAddress
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
	if _, err := c.conn.Read(header); err != nil {
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
	if _, err := c.conn.Read(fbData); err != nil {
		return 0, nil, fmt.Errorf("read fb data: %w", err)
	}

	return msgType, fbData, nil
}

// ExpectMsgType reads and verifies the message type is the expected one.
// Skips unexpected message types (push notifications from gateway) in a loop.
func (c *GatewayClient) ExpectMsgType(expected uint8, timeout time.Duration) ([]byte, error) {
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
	if _, err := conn.Read(lenBuf); err != nil {
		return nil, fmt.Errorf("read frame length: %w", err)
	}
	payloadLen := binary.BigEndian.Uint32(lenBuf)
	if payloadLen == 0 {
		return nil, nil
	}
	payload := make([]byte, payloadLen)
	if _, err := conn.Read(payload); err != nil {
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
	if _, err := r.conn.Read(header); err != nil {
		return 0, nil, fmt.Errorf("read frame header: %w", err)
	}
	payloadLen := binary.BigEndian.Uint32(header[0:4])
	msgType := header[4]
	payload := make([]byte, payloadLen)
	if payloadLen > 0 {
		if _, err := r.conn.Read(payload); err != nil {
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
