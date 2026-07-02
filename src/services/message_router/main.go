package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"os/signal"
	"syscall"
	"time"
)

func main() {
	// Early version check (before any initialization)
	for _, arg := range os.Args[1:] {
		if arg == "--version" || arg == "-v" {
			fmt.Println("MessageRouter Service (routerd)")
			fmt.Println("Version: (not configured - see main.go for setup instructions)")
			fmt.Println("Git Hash: (not configured)")
			fmt.Println("Build Date: (not configured)")
			os.Exit(0)
		}
	}

	port := flag.Int("port", 4000, "TCP listen port")
	flag.Parse()

	startTime := time.Now()

	router := NewRouter()
	router.StartCleanup()

	listener, err := net.Listen("tcp", fmt.Sprintf(":%d", *port))
	if err != nil {
		log.Fatalf("listen: %v", err)
	}

	log.Printf("message_router listening on :%d", *port)
	go handleSignals(listener, router, startTime, *port)

	for {
		conn, err := listener.Accept()
		if err != nil {
			log.Printf("accept: %v", err)
			continue
		}
		go handleConn(router, conn)
	}
}

func handleSignals(l net.Listener, r *Router, startTime time.Time, port int) {
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM, syscall.SIGUSR1)
	
	for {
		s := <-sig
		switch s {
		case syscall.SIGUSR1:
			// Print metrics
			uptime := time.Since(startTime)
			days := int(uptime.Hours() / 24)
			hours := int(uptime.Hours()) % 24
			minutes := int(uptime.Minutes()) % 60
			seconds := int(uptime.Seconds()) % 60
			
			log.Println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
			log.Println("METRICS: MessageRouter Service (routerd)")
			log.Println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
			log.Printf("Uptime: %d days, %02d:%02d:%02d", days, hours, minutes, seconds)
			log.Printf("TCP Port: %d", port)
			log.Printf("Active Clients: %d", r.ClientCount())
			log.Printf("Active Topics: %d", r.TopicCount())
			log.Println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
		case syscall.SIGINT, syscall.SIGTERM:
			log.Println("shutting down...")
			l.Close()
			os.Exit(0)
		}
	}
}

func handleConn(r *Router, conn net.Conn) {
	// Disable Nagle for low-latency message forwarding
	if tcpConn, ok := conn.(*net.TCPConn); ok {
		tcpConn.SetNoDelay(true)
	}
	cl := newClient(conn)
	defer func() {
		r.UnsubscribeAll(cl)
		cl.Close()
	}()

	log.Printf("[conn] new: %s", conn.RemoteAddr())

	buf := make([]byte, 64*1024)

	for {
		conn.SetReadDeadline(time.Now().Add(idleTimeout))
		msgType, payload, err := readFrame(conn, buf)
		if err != nil {
			if err != io.EOF && !isClosedConn(err) {
				log.Printf("[conn] read error: %s err=%v", conn.RemoteAddr(), err)
			}
			return
		}

		cl.lastSeen.Store(time.Now().UnixNano())

		switch msgType {
		case MsgSubscribe:
			topic, _, err := readString(payload, 0)
			if err != nil {
				log.Printf("[conn] bad subscribe frame: %s err=%v", conn.RemoteAddr(), err)
				continue
			}
			r.Subscribe(topic, cl)

		case MsgUnsubscribe:
			topic, _, err := readString(payload, 0)
			if err != nil {
				log.Printf("[conn] bad unsubscribe frame: %s err=%v", conn.RemoteAddr(), err)
				continue
			}
			r.Unsubscribe(topic, cl)

		case MsgPublish:
			topic, offset, err := readString(payload, 0)
			if err != nil {
				log.Printf("[conn] bad publish frame: %s err=%v", conn.RemoteAddr(), err)
				continue
			}
			data := payload[offset:]
			r.Publish(topic, data)

		case MsgRegister:
			name, offset, err := readString(payload, 0)
			if err != nil {
				log.Printf("[conn] bad register frame: %s err=%v", conn.RemoteAddr(), err)
				continue
			}
			if offset+2 > len(payload) {
				log.Printf("[conn] bad register frame (no topic count): %s", conn.RemoteAddr())
				continue
			}
			nTopics := int(binary.BigEndian.Uint16(payload[offset : offset+2]))
			offset += 2

			topics := make([]string, 0, nTopics)
			for i := 0; i < nTopics; i++ {
				topic, newOffset, err := readString(payload, offset)
				if err != nil {
					log.Printf("[conn] bad register frame (topic %d): %s err=%v", i, conn.RemoteAddr(), err)
					continue
				}
				topics = append(topics, topic)
				offset = newOffset
			}
			r.RegisterService(name, topics, cl)

		case MsgHeartbeat:

		default:
			log.Printf("[conn] unknown msg type %d from %s", msgType, conn.RemoteAddr())
		}
	}
}

func isClosedConn(err error) bool {
	if err == io.EOF {
		return true
	}
	if opErr, ok := err.(*net.OpError); ok {
		return opErr.Err.Error() == "use of closed network connection"
	}
	return false
}
