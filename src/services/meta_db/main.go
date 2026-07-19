package main

import (
	"encoding/json"
	"log"
	"net"
	"os"
	"os/signal"
	"syscall"
	"time"
)

// Local import - no network dependency
// Uses replace directive in go.mod for local build
import "github.com/gtnh/platform/gtnh-common/metrics"

const (
	dbPath = "metadb.sqlite"
	port   = ":5005"
)

func main() {
	metrics.CheckVersionAndExit("MetaDB Service (metadbd)")

	startTime := time.Now()

	m, err := NewMetaDB(dbPath)
	if err != nil {
		log.Fatalf("Failed to initialize MetaDB: %v", err)
	}
	defer m.db.Close()

	go startFlatBufferListener(m)

	routerClient := NewRouterClient(m)
	m.SetRouterClient(routerClient)
	routerClient.Start()
	defer routerClient.Stop()

	listener, err := net.Listen("tcp", port)
	if err != nil {
		log.Fatalf("Failed to listen on %s: %v", port, err)
	}
	defer listener.Close()

	// Setup signal handling early, separate from main loop
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGUSR1)
	go handleSignals(sig, m, startTime)

	log.Printf("MetaDB listening on %s", port)

	for {
		conn, err := listener.Accept()
		if err != nil {
			log.Printf("Accept error: %v", err)
			continue
		}
		go handleConnection(conn, m)
	}
}

func handleSignals(sig chan os.Signal, m *MetaDB, startTime time.Time) {
	for {
		<-sig
		playerCount := m.GetPlayerCount()
		
		metrics.PrintMetricsHeader("MetaDB Service (metadbd)")
		log.Printf("Uptime: %s", metrics.FormatUptime(startTime))
		log.Printf("Database: %s", dbPath)
		log.Printf("JSON RPC Port: %s", port)
		log.Printf("Player Count: %d", playerCount)
		metrics.PrintMetricsFooter()
	}
}

func handleConnection(conn net.Conn, m *MetaDB) {
	defer conn.Close()

	decoder := json.NewDecoder(conn)
	encoder := json.NewEncoder(conn)

	var req Request
	if err := decoder.Decode(&req); err != nil {
		log.Printf("Decode error: %v", err)
		return
	}

	resp := handleRequest(m, req)
	if err := encoder.Encode(resp); err != nil {
		log.Printf("Encode error: %v", err)
	}
}
