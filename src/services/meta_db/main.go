package main

import (
	"encoding/json"
	"fmt"
	"log"
	"net"
	"os"
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

	for {
		conn, err := listener.Accept()
		if err != nil {
			log.Printf("Accept error: %v", err)
			continue
		}
		go handleConnection(conn, m)
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
