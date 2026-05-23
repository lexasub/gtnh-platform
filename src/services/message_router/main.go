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
	port := flag.Int("port", 4000, "TCP listen port")
	flag.Parse()

	router := NewRouter()
	router.StartCleanup()

	listener, err := net.Listen("tcp", fmt.Sprintf(":%d", *port))
	if err != nil {
		log.Fatalf("listen: %v", err)
	}

	log.Printf("message_router listening on :%d", *port)
	go handleSignals(listener)

	for {
		conn, err := listener.Accept()
		if err != nil {
			log.Printf("accept: %v", err)
			continue
		}
		go handleConn(router, conn)
	}
}

func handleSignals(l net.Listener) {
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	<-sig
	log.Println("shutting down...")
	l.Close()
	os.Exit(0)
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
