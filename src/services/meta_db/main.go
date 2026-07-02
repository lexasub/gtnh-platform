package main

import (
	"encoding/json"
	"fmt"
	"log"
	"net"
	"os"
	"os/signal"
	"syscall"
	"time"
)

const (
	dbPath = "metadb.sqlite"
	port   = ":5005"
)

func main() {
	// Early version check (before any initialization)
	for _, arg := range os.Args[1:] {
		if arg == "--version" || arg == "-v" {
			fmt.Println("MetaDB Service (metadbd)")
			fmt.Println("Version: (not configured - see main.go for setup instructions)")
			fmt.Println("Git Hash: (not configured)")
			fmt.Println("Build Date: (not configured)")
			os.Exit(0)
		}
	}

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

	log.Printf("MetaDB listening on %s", port)

	// Handle signals in goroutine
	go handleSignals(m, startTime)

	for {
		conn, err := listener.Accept()
		if err != nil {
			log.Printf("Accept error: %v", err)
			continue
		}
		go handleConnection(conn, m)
	}
}

func handleSignals(m *MetaDB, startTime time.Time) {
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGUSR1)
	
	for {
		<-sig
		// Print metrics
		uptime := time.Since(startTime)
		days := int(uptime.Hours() / 24)
		hours := int(uptime.Hours()) % 24
		minutes := int(uptime.Minutes()) % 60
		seconds := int(uptime.Seconds()) % 60
		
		playerCount := m.GetPlayerCount()
		
		log.Println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
		log.Println("METRICS: MetaDB Service (metadbd)")
		log.Println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
		log.Printf("Uptime: %d days, %02d:%02d:%02d", days, hours, minutes, seconds)
		log.Printf("Database: %s", dbPath)
		log.Printf("JSON RPC Port: %s", port)
		log.Printf("Player Count: %d", playerCount)
		log.Println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
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
