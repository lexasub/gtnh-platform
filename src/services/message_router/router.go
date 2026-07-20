// MessageRouter: TCP pub/sub bus + service discovery.
//
// Wire frame format:
//   [4 bytes: payload length (big-endian)] [1 byte: message type] [payload]
//
// Message types:
//   0x01 Subscribe   — payload: [2 bytes topic len BE][topic]
//   0x02 Unsubscribe — payload: [2 bytes topic len BE][topic]
//   0x03 Publish     — payload: [2 bytes topic len BE][topic][opaque data]
//   0x04 Register    — payload: [2 bytes name len BE][name][2 bytes ntopics BE][topic...]
//   0x05 Heartbeat   — payload: none
//
// Topic patterns support MQTT-style wildcards:
//   '+' matches exactly one segment, '#' matches trailing segments, no wildcard = exact.
package main

import (
	"encoding/binary"
	"errors"
	"io"
	"log"
	"net"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

type MsgType byte

const (
	MsgSubscribe   MsgType = 0x01
	MsgUnsubscribe MsgType = 0x02
	MsgPublish     MsgType = 0x03
	MsgRegister    MsgType = 0x04
	MsgHeartbeat   MsgType = 0x05
)

const (
	frameHeaderSize = 5
	sendChSize      = 4096
	cleanupInterval = 120 * time.Second
	idleTimeout     = 60 * time.Second
)

var errShortFrame = errors.New("frame too short")

var framePool = sync.Pool{
	New: func() any {
		buf := make([]byte, 0, 64*1024)
		return &buf
	},
}

// ---------------------------------------------------------------------------
// Priority levels for subscriber send channels
// ---------------------------------------------------------------------------
//
// Each subscriber client has one send channel per priority level. A flood on
// a lower-priority topic (e.g. world.blocks.changed) cannot starve or drop
// messages on higher-priority topics (e.g. player.actions.ack).
//
// The writer goroutine uses a strict-priority cascade:
//
//	        ┌──────────┐
//	        │ PrioHigh │  ← player.actions.ack
//	        ├──────────┤
//	        │PriNormal │  ← world.*, player.inventory.*, player.joined, meta_db.inventory.*, metadb.player.online
//	        ├──────────┤
//	        │ PrioLow  │  ← entities.*, simulation.*, catch-all
//	        └──────────┘
//
//	PrioHigh messages are checked first at every opportunity,
//	so they are effectively never blocked behind lower priority traffic.

type Priority int

const (
	PrioHigh   Priority = 0 // player.actions.ack — must never be dropped
	PrioNormal Priority = 1 // world.*, player.inventory.*, player.joined, meta_db.inventory.*, metadb.player.online
	PrioLow    Priority = 2 // entities.*, simulation.*, catch-all

	PrioCount Priority = 3 // number of priority levels
)

// classifyTopic maps a topic to its delivery priority.
// Topics not explicitly listed default to PrioLow.
//
// PrioHigh: latency-sensitive, must never be delayed by chunk traffic.
// PrioNormal: bulk data (chunks, inventory).
// PrioLow: best-effort (entities, simulation internals).
func classifyTopic(topic string) Priority {
	switch {
	case topic == "player.actions.ack", topic == "player.action", topic == "player.setblock":
		return PrioHigh
	case strings.HasPrefix(topic, "world."):
		return PrioNormal
	case topic == "player.inventory.update", topic == "player.inventory.load", topic == "player.joined":
		return PrioNormal
	case topic == "meta_db.inventory.set", topic == "meta_db.inventory.get", topic == "metadb.player.online":
		return PrioNormal
	default:
		return PrioLow
	}
}

// ---------------------------------------------------------------------------
// client — per-connection state
// ---------------------------------------------------------------------------

type client struct {
	conn      net.Conn
	sendChs   []chan *[]byte
	lastSeen  atomic.Int64
	dropped   atomic.Uint64
	closeOnce sync.Once
	done      chan struct{} // closed by Close(); Publish selects on this to avoid send-on-closed
}

func newClient(conn net.Conn) *client {
	chs := make([]chan *[]byte, PrioCount)
	for i := range chs {
		chs[i] = make(chan *[]byte, sendChSize)
	}
	c := &client{
		conn:    conn,
		sendChs: chs,
		done:    make(chan struct{}),
	}
	c.lastSeen.Store(time.Now().UnixNano())
	go c.writer()
	return c
}

// writeBuf writes the frame bytes to the connection and returns the
// pooled buffer to framePool.  Must ONLY be called for frames allocated
// via allocPublishFrame (i.e. from framePool).
func (c *client) writeBuf(bp *[]byte) bool {
	if bp == nil || *bp == nil {
		return false
	}
	frame := *bp
	c.conn.SetWriteDeadline(time.Now().Add(30 * time.Second))
	if _, err := c.conn.Write(frame); err != nil {
		log.Printf("[writer] write error: %s err=%v", c.conn.RemoteAddr(), err)
		*bp = frame[:0]
		framePool.Put(bp)
		c.Close()
		return false
	}
	*bp = frame[:0]
	framePool.Put(bp)
	return true
}

// writer delivers frames from the priority send channels to the TCP
// connection using a strict-priority cascade:
//  1. Non-blocking check of PrioHigh only
//  2. Non-blocking check of PrioHigh + PrioNormal
//  3. Blocking check of PrioHigh + PrioNormal + PrioLow
//
// This ensures PrioHigh is always serviced first when data is available,
// while PrioLow only gets processed when higher channels are empty.
func (c *client) writer() {
	for {
		var bp *[]byte
		var ok bool
		select {
		case bp, ok = <-c.sendChs[PrioHigh]:
		default:
			select {
			case bp, ok = <-c.sendChs[PrioHigh]:
			case bp, ok = <-c.sendChs[PrioNormal]:
			default:
				select {
				case bp, ok = <-c.sendChs[PrioHigh]:
				case bp, ok = <-c.sendChs[PrioNormal]:
				case bp, ok = <-c.sendChs[PrioLow]:
				}
			}
		}
		if !ok {
			return
		}
		if !c.writeBuf(bp) {
			return
		}
	}
}

func (c *client) Close() {
	c.closeOnce.Do(func() {
		close(c.done)
		for _, ch := range c.sendChs {
			close(ch)
		}
		c.conn.Close()
	})
}

// ---------------------------------------------------------------------------
// ServiceInfo
// ---------------------------------------------------------------------------

type ServiceInfo struct {
	Name   string
	Topics []string
}

// ---------------------------------------------------------------------------
// Router — core pub/sub + service registry
// ---------------------------------------------------------------------------

type Router struct {
	mu           sync.RWMutex
	subs         map[string]map[*client]struct{}
	services     map[string]ServiceInfo
	connServices map[*client]string
}

func NewRouter() *Router {
	return &Router{
		subs:         make(map[string]map[*client]struct{}),
		services:     make(map[string]ServiceInfo),
		connServices: make(map[*client]string),
	}
}

func (r *Router) Subscribe(pattern string, cl *client) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.subs[pattern] == nil {
		r.subs[pattern] = make(map[*client]struct{})
	}
	r.subs[pattern][cl] = struct{}{}
	log.Printf("[router] subscribe: conn=%s pattern=%s", cl.conn.RemoteAddr(), pattern)
}

func (r *Router) Unsubscribe(pattern string, cl *client) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if subs, ok := r.subs[pattern]; ok {
		delete(subs, cl)
		if len(subs) == 0 {
			delete(r.subs, pattern)
		}
		log.Printf("[router] unsubscribe: conn=%s pattern=%s", cl.conn.RemoteAddr(), pattern)
	}
}

func (r *Router) UnsubscribeAll(cl *client) {
	r.mu.Lock()
	defer r.mu.Unlock()

	for pattern, subs := range r.subs {
		delete(subs, cl)
		if len(subs) == 0 {
			delete(r.subs, pattern)
		}
	}

	if name, ok := r.connServices[cl]; ok {
		delete(r.services, name)
		delete(r.connServices, cl)
		log.Printf("[router] unregister: conn=%s service=%s", cl.conn.RemoteAddr(), name)
	}

	log.Printf("[router] cleanup: conn=%s", cl.conn.RemoteAddr())
}

func (r *Router) RegisterService(name string, topics []string, cl *client) {
	r.mu.Lock()
	defer r.mu.Unlock()

	if prevName, ok := r.connServices[cl]; ok {
		delete(r.services, prevName)
	}

	r.services[name] = ServiceInfo{Name: name, Topics: topics}
	r.connServices[cl] = name

	for _, topic := range topics {
		if r.subs[topic] == nil {
			r.subs[topic] = make(map[*client]struct{})
		}
		r.subs[topic][cl] = struct{}{}
	}

	log.Printf("[router] register: conn=%s service=%s topics=%v", cl.conn.RemoteAddr(), name, topics)
}

// Publish fans out opaque data to all subscribers whose pattern matches topic.
//
// Messages are delivered on a per-subscriber, per-priority sendCh determined by
// classifyTopic(). PrioHigh uses blocking send — never drops, propagates
// backpressure to the publisher. PrioNormal/PrioLow use non-blocking send with
// drop-oldest when the channel is full.
//
// Because each priority level has its own sendCh, a flood on world.blocks.changed
// (PrioNormal) cannot cause ack messages (PrioHigh) to be dropped.
func (r *Router) Publish(topic string, data []byte) {
	r.mu.RLock()
	defer r.mu.RUnlock()

	matched, dropped := 0, 0
	prio := classifyTopic(topic)

	for pattern, clients := range r.subs {
		if !topicMatches(pattern, topic) {
			continue
		}
		for cl := range clients {
			bp := framePool.Get().(*[]byte)
			allocPublishFrame(bp, topic, data)

			// Use done channel to avoid send-on-closed-channel panic when
			// writeBuf calls Close() on write error while Publish holds only
			// an RLock on subs.
			select {
			case cl.sendChs[prio] <- bp:
				matched++
			case <-cl.done:
				dropped++
				cl.dropped.Add(1)
				*bp = (*bp)[:0]
				framePool.Put(bp)
			default:
				if prio == PrioHigh {
					// PrioHigh: blocking send without drop — but channel was
					// non-blocking-available AND client isn't done AND channel
					// isn't full — this shouldn't fire unless writer is
					// saturated. Re-select blocking.
					select {
					case cl.sendChs[prio] <- bp:
						matched++
					case <-cl.done:
						dropped++
						cl.dropped.Add(1)
						*bp = (*bp)[:0]
						framePool.Put(bp)
					}
				} else {
					// PrioNormal/PrioLow: drop-oldest on full channel
					select {
					case old := <-cl.sendChs[prio]:
						*old = (*old)[:0]
						framePool.Put(old)
						select {
						case cl.sendChs[prio] <- bp:
							matched++
						case <-cl.done:
							dropped++
							cl.dropped.Add(1)
							*bp = (*bp)[:0]
							framePool.Put(bp)
						default:
							dropped++
							cl.dropped.Add(1)
							*bp = (*bp)[:0]
							framePool.Put(bp)
						}
					case <-cl.done:
						dropped++
						cl.dropped.Add(1)
						*bp = (*bp)[:0]
						framePool.Put(bp)
					}
				}
			}
		}
	}

	if dropped > 0 {
		log.Printf("[router] publish: topic=%s bytes=%d delivered=%d dropped=%d (prio=%d)",
			topic, len(data), matched, dropped, prio)
	} else {
		log.Printf("[router] publish: topic=%s bytes=%d subscribers=%d (prio=%d)",
			topic, len(data), matched, prio)
	}
}

// StartCleanup runs a background loop that disconnects clients idle longer than timeout.
func (r *Router) StartCleanup() {
	go func() {
		ticker := time.NewTicker(cleanupInterval)
		defer ticker.Stop()
		for range ticker.C {
			r.cleanupOnce()
		}
	}()
}

func (r *Router) cleanupOnce() {
	r.mu.Lock()
	defer r.mu.Unlock()

	now := time.Now()
	seen := make(map[*client]struct{})

	for _, clients := range r.subs {
		for cl := range clients {
			if _, ok := seen[cl]; ok {
				continue
			}
			seen[cl] = struct{}{}
			lastSeen := time.Unix(0, cl.lastSeen.Load())
			if now.Sub(lastSeen) > idleTimeout {
				log.Printf("[router] idle timeout: conn=%s lastSeen=%s",
					cl.conn.RemoteAddr(), lastSeen.Format(time.RFC3339))
				r.UnsubscribeAll(cl)
				cl.Close()
			}
		}
	}
}

// Services returns a copy of all registered services.
func (r *Router) Services() map[string]ServiceInfo {
	r.mu.RLock()
	defer r.mu.RUnlock()
	cp := make(map[string]ServiceInfo, len(r.services))
	for k, v := range r.services {
		cp[k] = v
	}
	return cp
}

// ---------------------------------------------------------------------------
// Topic matching (MQTT-style)
// ---------------------------------------------------------------------------

func topicMatches(pattern, topic string) bool {
	if !strings.ContainsAny(pattern, "+#") {
		return pattern == topic
	}

	pSegs := strings.Split(pattern, ".")
	tSegs := strings.Split(topic, ".")

	pi, ti := 0, 0
	for pi < len(pSegs) && ti < len(tSegs) {
		if pSegs[pi] == "#" {
			return true
		}
		if pSegs[pi] != "+" && pSegs[pi] != tSegs[ti] {
			return false
		}
		pi++
		ti++
	}

	if pi == len(pSegs) && ti == len(tSegs) {
		return true
	}
	if pi == len(pSegs)-1 && pSegs[pi] == "#" && ti == len(tSegs) {
		return true
	}
	return false
}

// ---------------------------------------------------------------------------
// Frame I/O
// ---------------------------------------------------------------------------

// readFrame reads one frame from conn using buf as scratch space.
// buf is NOT safe for concurrent use — caller must own it (one goroutine).
func readFrame(conn net.Conn, buf []byte) (MsgType, []byte, error) {
	header := buf[:frameHeaderSize]
	if _, err := io.ReadFull(conn, header); err != nil {
		return 0, nil, err
	}

	payloadLen := binary.BigEndian.Uint32(header[:4])
	if payloadLen < 1 {
		return 0, nil, errShortFrame
	}

	msgType := MsgType(header[4])

	var payload []byte
	totalLen := int(payloadLen) - 1
	if totalLen > 0 {
		if totalLen <= cap(buf)-frameHeaderSize {
			payload = buf[frameHeaderSize : frameHeaderSize+totalLen]
		} else {
			payload = make([]byte, totalLen)
		}
		if _, err := io.ReadFull(conn, payload); err != nil {
			return 0, nil, err
		}
	}

	return msgType, payload, nil
}

// allocPublishFrame fills a pooled buffer with a publish frame.
// Caller owns the returned slice; must return *buf to framePool when done.
func allocPublishFrame(buf *[]byte, topic string, data []byte) []byte {
	topicBytes := []byte(topic)
	// payload_len = type(1) + topic_len_prefix(2) + topic + data
	payloadLen := 1 + 2 + len(topicBytes) + len(data)
	frameSize := 4 + payloadLen // 4-byte length header + payload

	if cap(*buf) < frameSize {
		*buf = make([]byte, frameSize)
	}
	*buf = (*buf)[:frameSize]
	frame := *buf

	binary.BigEndian.PutUint32(frame[0:4], uint32(payloadLen))
	frame[4] = byte(MsgPublish)
	binary.BigEndian.PutUint16(frame[5:7], uint16(len(topicBytes)))
	copy(frame[7:], topicBytes)
	copy(frame[7+len(topicBytes):], data)

	return frame
}

func makeFrame(msgType MsgType, payload []byte) []byte {
	payloadLen := 1 + len(payload)
	frame := make([]byte, frameHeaderSize+payloadLen)

	binary.BigEndian.PutUint32(frame[0:4], uint32(payloadLen))
	frame[4] = byte(msgType)
	copy(frame[5:], payload)

	return frame
}

func readString(payload []byte, offset int) (string, int, error) {
	if offset+2 > len(payload) {
		return "", 0, errShortFrame
	}
	strLen := int(binary.BigEndian.Uint16(payload[offset : offset+2]))
	offset += 2
	if offset+strLen > len(payload) {
		return "", 0, errShortFrame
	}
	return string(payload[offset : offset+strLen]), offset + strLen, nil
}
