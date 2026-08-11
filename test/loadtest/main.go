package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"math"
	"net"
	"sync"
	"sync/atomic"
	"time"

	"github.com/google/flatbuffers/go"
	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
)

const (
	kPlayerAction = 1
	kBlockAck     = 5
)

var (
	ctrlAddr = flag.String("ctrl", "127.0.0.1:7777", "Gateway ctrl address")
	rate     = flag.Int("rate", 50, "Actions per second")
	duration = flag.Int("duration", 10, "Test duration in seconds")
	posX     = flag.Int("x", 256, "Block X position")
	posY     = flag.Int("y", 17, "Block Y position")
	posZ     = flag.Int("z", 138, "Block Z position")
)

type stats struct {
	sent     int32
	received int32
	latency  []float64
	mu       sync.Mutex
}

func (s *stats) addLatency(d time.Duration) {
	s.mu.Lock()
	s.latency = append(s.latency, d.Seconds()*1000)
	s.mu.Unlock()
}

func (s *stats) report() {
	s.mu.Lock()
	defer s.mu.Unlock()
	n := len(s.latency)
	if n == 0 {
		fmt.Println("\nNo responses received.")
		return
	}
	var sum, min, max float64
	min = math.MaxFloat64
	for _, v := range s.latency {
		sum += v
		if v < min {
			min = v
		}
		if v > max {
			max = v
		}
	}
	avg := sum / float64(n)
	// Sort for percentiles
	sort := make([]float64, n)
	copy(sort, s.latency)
	for i := 0; i < n; i++ {
		for j := i + 1; j < n; j++ {
			if sort[j] < sort[i] {
				sort[i], sort[j] = sort[j], sort[i]
			}
		}
	}
	p50 := sort[n*50/100]
	p95 := sort[n*95/100]
	p99 := sort[n*99/100]

	fmt.Printf("\n=== Load Test Results ===\n")
	fmt.Printf("Sent: %d, Received: %d (%.1f%%)\n", s.sent, s.received, float64(s.received)/float64(s.sent)*100)
	fmt.Printf("Latency (ms): min=%.2f avg=%.2f max=%.2f\n", min, avg, max)
	fmt.Printf("Percentiles (ms): p50=%.2f p95=%.2f p99=%.2f\n", p50, p95, p99)
}

func main() {
	flag.Parse()

	fmt.Printf("Connecting to Gateway ctrl at %s...\n", *ctrlAddr)
	conn, err := net.DialTimeout("tcp", *ctrlAddr, 5*time.Second)
	if err != nil {
		fmt.Printf("Connect failed: %v\n", err)
		return
	}
	defer conn.Close()
	fmt.Println("Connected.")

	var st stats
	var nextSeq uint64
	type pendingKey struct{ x, y, z int32 }
	pending := make(map[pendingKey]time.Time)

	// Reader goroutine
	go func() {
		header := make([]byte, 5)
		for {
			if _, err := conn.Read(header); err != nil {
				return
			}
			payloadLen := binary.BigEndian.Uint32(header[:4])
			if payloadLen < 1 {
				continue
			}
			msgType := header[4]
			totalLen := int(payloadLen) - 1
			if totalLen <= 0 {
				continue
			}
			payload := make([]byte, totalLen)
			if _, err := conn.Read(payload); err != nil {
				return
			}

			if msgType == kBlockAck {
				ack := Protocol.GetRootAsBlockAck(payload, 0)
				var pos Protocol.Vec3i
				ack.Pos(&pos)
				status := ack.Status()

				key := pendingKey{pos.X(), pos.Y(), pos.Z()}
				if t, ok := pending[key]; ok {
					delete(pending, key)
					st.addLatency(time.Since(t))
				}

				statusStr := "OK"
				if status == 1 {
					statusStr = "CONFLICT"
				}
				recv := atomic.AddInt32(&st.received, 1)
				fmt.Printf("  pos=(%d,%d,%d) %s  recv=%d\n",
					pos.X(), pos.Y(), pos.Z(), statusStr, recv)
			}
		}
	}()

	// Let connection settle before blasting
	time.Sleep(500 * time.Millisecond)

	// Sender loop
	ticker := time.NewTicker(time.Second / time.Duration(*rate))
	defer ticker.Stop()

	timeout := time.After(time.Duration(*duration) * time.Second)
	done := make(chan struct{})

	go func() {
		<-timeout
		close(done)
	}()

loop:
	for {
		select {
		case <-ticker.C:
			seq := nextSeq
			nextSeq++
			x := int32(*posX) + int32(seq%32)
			y := int32(*posY)
			z := int32(*posZ) + int32((seq/32)%32)

			// Build PlayerAction FlatBuffer
			builder := flatbuffers.NewBuilder(64)
			Protocol.SetBlockActionStart(builder)
			Protocol.SetBlockActionAddPlayerId(builder, 0)
			Protocol.SetBlockActionAddAction(builder, Protocol.PlayerActionTypeLEFT_MOUSE_CLICK)
			pos := Protocol.CreateVec3i(builder, x, y, z)
			Protocol.SetBlockActionAddPos(builder, pos)
			Protocol.SetBlockActionAddExpectedBlockId(builder, 0)
			Protocol.SetBlockActionAddNewBlockId(builder, 0)
			action := Protocol.SetBlockActionEnd(builder)
			builder.Finish(action)

			fbData := builder.FinishedBytes()

			totalLen := 1 + len(fbData)
			frame := make([]byte, 4+totalLen)
			binary.BigEndian.PutUint32(frame[0:4], uint32(totalLen))
			frame[4] = kPlayerAction
			copy(frame[5:], fbData)

			pending[pendingKey{x, y, z}] = time.Now()
			if _, err := conn.Write(frame); err != nil {
				fmt.Printf("\nWrite error: %v\n", err)
				break loop
			}
			atomic.AddInt32(&st.sent, 1)

		case <-done:
			break loop
		}
	}

	fmt.Println("\n\nWaiting for trailing responses...")
	time.Sleep(2 * time.Second)

	st.report()
}
