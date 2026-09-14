// MessageRouter: TCP pub/sub bus + service discovery.
//
// Wire frame format:
//
//	[4 bytes: payload length (big-endian)] [1 byte: message type] [payload]
//
// Message types:
//
//	0x01 Subscribe, 0x02 Unsubscribe, 0x03 Publish, 0x04 Register,
//	0x05 Heartbeat, 0x06 HealthRequest, 0x07 HealthResponse.
package main

import (
	"encoding/binary"
	"errors"
	"io"
	"log"
	"net"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

type MsgType byte

const (
	MsgSubscribe      MsgType = 0x01
	MsgUnsubscribe    MsgType = 0x02
	MsgPublish        MsgType = 0x03
	MsgRegister       MsgType = 0x04
	MsgHeartbeat      MsgType = 0x05
	MsgHealthRequest  MsgType = 0x06
	MsgHealthResponse MsgType = 0x07
)

const (
	frameHeaderSize    = 5
	sendChSize         = 4096
	cleanupInterval    = 120 * time.Second
	idleTimeout        = 60 * time.Second
	healthProbeTimeout = 2 * time.Second
)

var errShortFrame = errors.New("frame too short")

var framePool = sync.Pool{New: func() any {
	buf := make([]byte, 0, 64*1024)
	return &buf
}}

type Priority int

const (
	PrioHigh   Priority = 0
	PrioNormal Priority = 1
	PrioLow    Priority = 2
	PrioCount  Priority = 3
)

func classifyTopic(topic string) Priority {
	switch {
	case topic == "player.actions.ack", topic == "player.actions":
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

type client struct {
	conn      net.Conn
	sendChs   []chan *[]byte
	lastSeen  atomic.Int64
	dropped   atomic.Uint64
	closeOnce sync.Once
	done      chan struct{}
}

func newClient(conn net.Conn) *client {
	chs := make([]chan *[]byte, PrioCount)
	for i := range chs {
		chs[i] = make(chan *[]byte, sendChSize)
	}
	cl := &client{conn: conn, sendChs: chs, done: make(chan struct{})}
	cl.lastSeen.Store(time.Now().UnixNano())
	go cl.writer()
	return cl
}

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
		if !ok || !c.writeBuf(bp) {
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

type ServiceInfo struct {
	Name   string
	Topics []string
}

type healthStatus struct {
	transportAlive bool
	probeState     byte
	lastResponse   time.Time
}

type healthRow struct {
	name           string
	transportAlive bool
	probeState     byte
	ageMs          uint64
}

type healthProbe struct {
	requester *client
	services  map[*client]string
	responses map[*client]time.Time
}

type Router struct {
	mu           sync.RWMutex
	subs         map[string]map[*client]struct{}
	services     map[string]ServiceInfo
	connServices map[*client]string
	healthMu     sync.Mutex
	health       map[string]healthStatus
	healthProbes map[uint64]*healthProbe
}

func NewRouter() *Router {
	return &Router{
		subs:         make(map[string]map[*client]struct{}),
		services:     make(map[string]ServiceInfo),
		connServices: make(map[*client]string),
		health:       make(map[string]healthStatus),
		healthProbes: make(map[uint64]*healthProbe),
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
	r.unsubscribeAllLocked(cl)
}

func (r *Router) unsubscribeAllLocked(cl *client) {
	for pattern, subs := range r.subs {
		delete(subs, cl)
		if len(subs) == 0 {
			delete(r.subs, pattern)
		}
	}
	if name, ok := r.connServices[cl]; ok {
		delete(r.services, name)
		delete(r.connServices, cl)
		r.healthMu.Lock()
		delete(r.health, name)
		r.healthMu.Unlock()
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
	r.healthMu.Lock()
	r.health[name] = healthStatus{transportAlive: true}
	r.healthMu.Unlock()
	for _, topic := range topics {
		if r.subs[topic] == nil {
			r.subs[topic] = make(map[*client]struct{})
		}
		r.subs[topic][cl] = struct{}{}
	}
	log.Printf("[router] register: conn=%s service=%s topics=%v", cl.conn.RemoteAddr(), name, topics)
}

func makeControlFrame(msgType MsgType, payload []byte) *[]byte {
	frame := make([]byte, frameHeaderSize+1+len(payload))
	binary.BigEndian.PutUint32(frame[:4], uint32(1+len(payload)))
	frame[4] = byte(msgType)
	copy(frame[5:], payload)
	return &frame
}

func encodeNonce(nonce uint64) []byte {
	payload := make([]byte, 8)
	binary.BigEndian.PutUint64(payload, nonce)
	return payload
}

func encodeHealthSnapshot(nonce uint64, rows []healthRow) []byte {
	payload := make([]byte, 10)
	binary.BigEndian.PutUint64(payload[:8], nonce)
	binary.BigEndian.PutUint16(payload[8:10], uint16(len(rows)))
	for _, row := range rows {
		name := []byte(row.name)
		entry := make([]byte, 2+len(name)+10)
		binary.BigEndian.PutUint16(entry[:2], uint16(len(name)))
		copy(entry[2:], name)
		entry[2+len(name)] = row.probeState
		if row.transportAlive {
			entry[3+len(name)] = 1
		}
		binary.BigEndian.PutUint64(entry[4+len(name):], row.ageMs)
		payload = append(payload, entry...)
	}
	return payload
}

func (r *Router) healthSnapshot(requester *client, nonce uint64) {
	log.Printf("[health] snapshot request nonce=%d services=%d", nonce, len(r.connServices))
	r.mu.RLock()
	services := make(map[*client]string, len(r.connServices))
	for cl, name := range r.connServices {
		services[cl] = name
	}
	r.mu.RUnlock()

	probe := &healthProbe{requester: requester, services: services, responses: make(map[*client]time.Time)}
	r.healthMu.Lock()
	r.healthProbes[nonce] = probe
	r.healthMu.Unlock()
	for cl := range services {
		if cl == requester {
			// The gateway is the requester, so the successful snapshot exchange
			// itself proves that its Router transport and handler are alive.
			probe.responses[cl] = time.Now()
			continue
		}
		select {
		case cl.sendChs[PrioHigh] <- makeControlFrame(MsgHealthRequest, encodeNonce(nonce)):
		case <-cl.done:
		}
	}
	go func() {
		time.Sleep(healthProbeTimeout)
		r.finishHealthProbe(nonce)
	}()
}

func (r *Router) finishHealthProbe(nonce uint64) {
	r.healthMu.Lock()
	probe, ok := r.healthProbes[nonce]
	if !ok {
		r.healthMu.Unlock()
		return
	}
	delete(r.healthProbes, nonce)
	now := time.Now()
	rows := make([]healthRow, 0, len(probe.services))
	services := make([]struct {
		cl   *client
		name string
	}, 0, len(probe.services))
	for cl, name := range probe.services {
		services = append(services, struct {
			cl   *client
			name string
		}{cl: cl, name: name})
	}
	sort.Slice(services, func(i, j int) bool { return services[i].name < services[j].name })
	for _, service := range services {
		cl, name := service.cl, service.name
		status := r.health[name]
		status.transportAlive = r.clientAlive(cl)
		if responseAt, ok := probe.responses[cl]; ok {
			status.probeState = 1
			status.lastResponse = responseAt
		} else {
			status.probeState = 2
		}
		r.health[name] = status
		age := uint64(0)
		if !status.lastResponse.IsZero() {
			age = uint64(now.Sub(status.lastResponse).Milliseconds())
		}
		rows = append(rows, healthRow{name: name, transportAlive: status.transportAlive, probeState: status.probeState, ageMs: age})
	}
	r.healthMu.Unlock()
	select {
	case probe.requester.sendChs[PrioHigh] <- makeControlFrame(MsgHealthResponse, encodeHealthSnapshot(nonce, rows)):
	case <-probe.requester.done:
	}
}

func (r *Router) clientAlive(cl *client) bool {
	select {
	case <-cl.done:
		return false
	default:
		return true
	}
}

func (r *Router) serviceName(cl *client) (string, bool) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	name, ok := r.connServices[cl]
	return name, ok
}

func (r *Router) handleHealthRequest(cl *client, payload []byte) {
	if len(payload) != 8 {
		return
	}
	nonce := binary.BigEndian.Uint64(payload)
	name, registered := r.serviceName(cl)
	log.Printf("[health] request nonce=%d requester=%s registered=%v", nonce, name, registered)
	if registered && name != "gateway" {
		select {
		case cl.sendChs[PrioHigh] <- makeControlFrame(MsgHealthResponse, payload):
		case <-cl.done:
		}
		return
	}
	r.healthSnapshot(cl, nonce)
}

func (r *Router) handleHealthResponse(cl *client, payload []byte) {
	if len(payload) != 8 {
		return
	}
	nonce := binary.BigEndian.Uint64(payload)
	r.healthMu.Lock()
	if probe, ok := r.healthProbes[nonce]; ok {
		probe.responses[cl] = time.Now()
	}
	r.healthMu.Unlock()
}

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
		log.Printf("[router] publish: topic=%s bytes=%d delivered=%d dropped=%d (prio=%d)", topic, len(data), matched, dropped, prio)
	}
}

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
			if _, ok := r.connServices[cl]; ok {
				continue
			}
			if now.Sub(time.Unix(0, cl.lastSeen.Load())) > idleTimeout {
				r.unsubscribeAllLocked(cl)
				cl.Close()
			}
		}
	}
}

func (r *Router) Services() map[string]ServiceInfo {
	r.mu.RLock()
	defer r.mu.RUnlock()
	cp := make(map[string]ServiceInfo, len(r.services))
	for k, v := range r.services {
		cp[k] = v
	}
	return cp
}

func (r *Router) ClientCount() int {
	r.mu.RLock()
	defer r.mu.RUnlock()
	seen := make(map[*client]struct{})
	for _, clients := range r.subs {
		for cl := range clients {
			seen[cl] = struct{}{}
		}
	}
	return len(seen)
}

func (r *Router) TopicCount() int { r.mu.RLock(); defer r.mu.RUnlock(); return len(r.subs) }

func topicMatches(pattern, topic string) bool {
	if !strings.ContainsAny(pattern, "+#") {
		return pattern == topic
	}
	pSegs, tSegs := strings.Split(pattern, "."), strings.Split(topic, ".")
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
	return (pi == len(pSegs) && ti == len(tSegs)) || (pi == len(pSegs)-1 && pSegs[pi] == "#" && ti == len(tSegs))
}

func readFrame(conn net.Conn, buf []byte) (MsgType, []byte, error) {
	if len(buf) < frameHeaderSize {
		return 0, nil, errShortFrame
	}
	if _, err := io.ReadFull(conn, buf[:frameHeaderSize]); err != nil {
		return 0, nil, err
	}
	payloadLen := binary.BigEndian.Uint32(buf[:4])
	if payloadLen < 1 {
		return 0, nil, errShortFrame
	}
	msgType := MsgType(buf[4])
	totalLen := int(payloadLen) - 1
	if totalLen <= 0 {
		return msgType, nil, nil
	}
	var payload []byte
	if totalLen <= cap(buf)-frameHeaderSize {
		payload = buf[frameHeaderSize : frameHeaderSize+totalLen]
	} else {
		payload = make([]byte, totalLen)
	}
	if _, err := io.ReadFull(conn, payload); err != nil {
		return 0, nil, err
	}
	return msgType, payload, nil
}

func allocPublishFrame(buf *[]byte, topic string, data []byte) []byte {
	topicBytes := []byte(topic)
	payloadLen := 1 + 2 + len(topicBytes) + len(data)
	frameSize := 4 + payloadLen
	if cap(*buf) < frameSize {
		*buf = make([]byte, frameSize)
	}
	*buf = (*buf)[:frameSize]
	frame := *buf
	binary.BigEndian.PutUint32(frame[:4], uint32(payloadLen))
	frame[4] = byte(MsgPublish)
	binary.BigEndian.PutUint16(frame[5:7], uint16(len(topicBytes)))
	copy(frame[7:], topicBytes)
	copy(frame[7+len(topicBytes):], data)
	return frame
}

func makeFrame(msgType MsgType, payload []byte) []byte {
	payloadLen := 1 + len(payload)
	frame := make([]byte, frameHeaderSize+payloadLen)
	binary.BigEndian.PutUint32(frame[:4], uint32(payloadLen))
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
