package main

import (
	"encoding/binary"
	"io"
	"log"
	"net"
	"sync"
	"time"

	"github.com/gtnh-platform/protocol/generated/go/Protocol"
)

// ---------------------------------------------------------------------------
// MessageRouter wire protocol (mirrors router.go)
// ---------------------------------------------------------------------------

type msgType byte

const (
	msgSubscribe   msgType = 0x01
	msgUnsubscribe msgType = 0x02
	msgPublish     msgType = 0x03
	msgRegister    msgType = 0x04
	msgHeartbeat   msgType = 0x05
)

const routerAddr = "127.0.0.1:4000"

// ---------------------------------------------------------------------------
// RouterClient
// ---------------------------------------------------------------------------

// RouterClient connects MetaDB to the Go MessageRouter (TCP pub/sub bus).
// It registers as the "metadb" service, subscribes to request topics,
// and handles incoming RPC publishes by delegating to the FlatBuffers
// handler logic. Responses are published back on a reply topic.
type RouterClient struct {
	m    *MetaDB
	conn net.Conn
	mu   sync.Mutex
	done chan struct{}
}

func NewRouterClient(m *MetaDB) *RouterClient {
	return &RouterClient{
		m:    m,
		done: make(chan struct{}),
	}
}

func (rc *RouterClient) Start() {
	go rc.loop()
}

func (rc *RouterClient) Stop() {
	close(rc.done)
	rc.mu.Lock()
	if rc.conn != nil {
		rc.conn.Close()
	}
	rc.mu.Unlock()
}

func (rc *RouterClient) loop() {
	for {
		select {
		case <-rc.done:
			return
		default:
		}

		err := rc.connectAndServe()
		if err != nil {
			log.Printf("[router] disconnected: %v — reconnecting in 1s", err)
		}

		select {
		case <-rc.done:
			return
		case <-time.After(1 * time.Second):
		}
	}
}

func (rc *RouterClient) connectAndServe() error {
	conn, err := net.Dial("tcp", routerAddr)
	if err != nil {
		return err
	}

	rc.mu.Lock()
	rc.conn = conn
	rc.mu.Unlock()

	log.Printf("[router] connected to MessageRouter on %s", routerAddr)

	// Register as "metadb" service with topics
	// RegisterService subscribes to all listed topics — no separate Subscribe needed.
	topics := []string{
		"meta_db.inventory.get",
		"meta_db.inventory.set",
		"meta_db.inventory.snapshot",
		"player.joined",
		"player.left",
	}
	writeRegister(conn, "metadb", topics)

	writePublish(conn, "metadb.player.online", nil)

	// Heartbeat ticker (every 20s, router times out at 60s)
	hbTicker := time.NewTicker(20 * time.Second)
	defer hbTicker.Stop()

	// Read loop goroutine
	type frame struct {
		mt msgType
		p  []byte
	}
	frameCh := make(chan frame, 64)

	go func() {
		buf := make([]byte, 64*1024)
		for {
			mt, payload, err := readRouterFrame(conn, buf)
			if err != nil {
				log.Printf("[router] read error: %v", err)
				close(frameCh)
				return
			}
			log.Printf("[router] RX frame: mt=%d payload_len=%d", mt, len(payload))
			frameCh <- frame{msgType(mt), payload}
		}
	}()

	for {
		select {
		case <-rc.done:
			conn.Close()
			return nil
		case <-hbTicker.C:
			if err := writeHeartbeat(conn); err != nil {
				log.Printf("[router] heartbeat write failed: %v — reconnecting", err)
				conn.Close()
				return err
			}
		case f, ok := <-frameCh:
			if !ok {
				return io.EOF
			}
			rc.handleRouterFrame(f.mt, f.p)
		}
	}
}

func (rc *RouterClient) handleRouterFrame(mt msgType, payload []byte) {
	switch mt {
	case msgPublish:
		rc.handlePublish(payload)
	case msgHeartbeat:
		// router sends heartbeat to check liveness; nothing to do
	default:
		log.Printf("[router] unexpected message type %d", mt)
	}
}

// handlePublish processes an incoming publish from the router.
// Payload format: [2 bytes topic len BE][topic][opaque FlatBuffer data]
func (rc *RouterClient) handlePublish(payload []byte) {
	defer func() {
		if r := recover(); r != nil {
			log.Printf("[router] PANIC in handlePublish: %v", r)
		}
	}()
	if len(payload) < 2 {
		return
	}
	topicLen := int(binary.BigEndian.Uint16(payload[:2]))
	if 2+topicLen > len(payload) {
		return
	}
	topic := string(payload[2 : 2+topicLen])
	fbData := payload[2+topicLen:]

	if topic == "meta_db.inventory.set" {
		maxHex := len(fbData)
		if maxHex > 48 { maxHex = 48 }
		log.Printf("[router] meta_db.inventory.set: fb_len=%d hex=%x", len(fbData), fbData[:maxHex])
	}

	// Route new event topics directly (no MetaDBFrame wrapper)
	switch topic {
	case "player.joined":
		handlePlayerJoined(fbData, rc.m)
		return
	case "player.left":
		handlePlayerLeft(fbData, rc.m)
		return
	}

	// Existing MetaDBFrame-based RPC topics
	frame := Protocol.GetRootAsMetaDBFrame(fbData, 0)
	if frame == nil {
		log.Printf("[router] GetRootAsMetaDBFrame returned nil for topic=%s fb_len=%d", topic, len(fbData))
		return
	}

	respData := dispatchFlatBufferFrame(frame, rc.m)
	if respData == nil {
		log.Printf("[router] dispatchFlatBufferFrame returned nil for topic=%s", topic)
		return
	}

	// Publish response back on reply topic
	replyTopic := topic + ".response"
	writePublish(rc.conn, replyTopic, respData)

	log.Printf("[router] handled %s → %s (%d bytes)", topic, replyTopic, len(respData))
}

// ---------------------------------------------------------------------------
// Frame I/O helpers (matching router.go protocol)
// ---------------------------------------------------------------------------

// readRouterFrame reads one frame from conn.
func readRouterFrame(conn net.Conn, buf []byte) (msgType, []byte, error) {
	header := buf[:5]
	if _, err := io.ReadFull(conn, header); err != nil {
		return 0, nil, err
	}

	payloadLen := binary.BigEndian.Uint32(header[:4])
	if payloadLen < 1 {
		return 0, nil, io.ErrUnexpectedEOF
	}

	mt := msgType(header[4])

	var payload []byte
	totalLen := int(payloadLen) - 1
	if totalLen > 0 {
		if totalLen <= cap(buf)-5 {
			payload = buf[5 : 5+totalLen]
		} else {
			payload = make([]byte, totalLen)
		}
		if _, err := io.ReadFull(conn, payload); err != nil {
			return 0, nil, err
		}
	}

	return mt, payload, nil
}

// writeFrame sends a raw frame to conn.
func writeFrame(conn net.Conn, mt msgType, payload []byte) error {
	payloadLen := 1 + len(payload)
	frame := make([]byte, 4+payloadLen)

	binary.BigEndian.PutUint32(frame[:4], uint32(payloadLen))
	frame[4] = byte(mt)
	copy(frame[5:], payload)

	_, err := conn.Write(frame)
	return err
}

// writeRegister sends a Register frame.
// Payload: [2 bytes name len BE][name][2 bytes ntopics BE][topic...]
func writeRegister(conn net.Conn, name string, topics []string) {
	payloadLen := 2 + len(name) + 2
	for _, t := range topics {
		payloadLen += 2 + len(t)
	}

	payload := make([]byte, payloadLen)
	off := 0
	binary.BigEndian.PutUint16(payload[off:], uint16(len(name)))
	off += 2
	copy(payload[off:], name)
	off += len(name)
	binary.BigEndian.PutUint16(payload[off:], uint16(len(topics)))
	off += 2
	for _, t := range topics {
		binary.BigEndian.PutUint16(payload[off:], uint16(len(t)))
		off += 2
		copy(payload[off:], t)
		off += len(t)
	}

	if err := writeFrame(conn, msgRegister, payload); err != nil {
		log.Printf("[router] register error: %v", err)
	}
}

// writeSubscribe sends a Subscribe frame.
// Payload: [2 bytes topic len BE][topic]
func writeSubscribe(conn net.Conn, topic string) {
	payload := make([]byte, 2+len(topic))
	binary.BigEndian.PutUint16(payload[:2], uint16(len(topic)))
	copy(payload[2:], topic)

	if err := writeFrame(conn, msgSubscribe, payload); err != nil {
		log.Printf("[router] subscribe error: %v", err)
	}
}

// writePublish sends a Publish frame.
// Payload: [2 bytes topic len BE][topic][opaque data]
func writePublish(conn net.Conn, topic string, data []byte) {
	payload := make([]byte, 2+len(topic)+len(data))
	off := 0
	binary.BigEndian.PutUint16(payload[off:], uint16(len(topic)))
	off += 2
	copy(payload[off:], topic)
	off += len(topic)
	copy(payload[off:], data)

	if err := writeFrame(conn, msgPublish, payload); err != nil {
		log.Printf("[router] publish error: %v", err)
	}
}

// PublishRaw publishes raw data to a topic via the router connection.
func (rc *RouterClient) PublishRaw(topic string, data []byte) {
	rc.mu.Lock()
	conn := rc.conn
	rc.mu.Unlock()
	if conn == nil {
		return
	}
	writePublish(conn, topic, data)
}

// writeHeartbeat sends a Heartbeat frame (empty payload).
func writeHeartbeat(conn net.Conn) error {
	conn.SetWriteDeadline(time.Now().Add(10 * time.Second))
	return writeFrame(conn, msgHeartbeat, nil)
}

// ---------------------------------------------------------------------------
// Event handlers (fire-and-forget topics, not RPC)
// ---------------------------------------------------------------------------

func handlePlayerJoined(data []byte, m *MetaDB) {
	joined := Protocol.GetRootAsPlayerJoined(data, 0)
	playerID := joined.PlayerId()
	log.Printf("[router] player.joined: id=%d", playerID)

	slots, err := m.GetInventory(playerID)
	if err != nil {
		log.Printf("[router] player.joined: GetInventory error: %v", err)
		return
	}
	if len(slots) == 0 {
		log.Printf("[router] player.joined: no saved inventory for player %d, skipping publish", playerID)
		return
	}
	m.PublishInventoryTo("player.inventory.load", playerID, slots)
}

func handlePlayerLeft(data []byte, m *MetaDB) {
	left := Protocol.GetRootAsPlayerLeft(data, 0)
	playerID := left.PlayerId()
	x := left.X()
	y := left.Y()
	z := left.Z()
	log.Printf("[router] player.left: id=%d pos=[%d,%d,%d]", playerID, x, y, z)

	if err := m.SavePlayerPosition(playerID, int(x), int(y), int(z)); err != nil {
		log.Printf("[router] player.left: SavePlayerPosition error: %v", err)
	}
}



