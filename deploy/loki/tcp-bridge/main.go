package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"sync"
	"time"
)

// Loki push request: https://grafana.com/docs/loki/latest/reference/api/#push-log-entries-to-loki
type stream struct {
	Stream map[string]string `json:"stream"`
	Values [][2]string       `json:"values"` // [timestamp_ns, line]
}

type pushReq struct {
	Streams []stream `json:"streams"`
}

const flushInterval = 1 * time.Second
const flushMaxLines = 500

var (
	listenAddr = flag.String("listen", ":1514", "TCP listen address")
	lokiURL    = flag.String("loki", "http://127.0.0.1:13100/loki/api/v1/push", "Loki push API URL")
	svcLabel   = flag.String("label", "job", "Loki label key for source")
	hostLabel  = flag.String("host", "gtnh-bridge", "Loki label value for hostname")
)

type lineEntry struct {
	line string
	time time.Time
}

func main() {
	flag.Parse()

	ln, err := net.Listen("tcp", *listenAddr)
	if err != nil {
		log.Fatalf("listen %s: %v", *listenAddr, err)
	}
	log.Printf("TCP→Loki bridge listening on %s, pushing to %s", *listenAddr, *lokiURL)

	var mu sync.Mutex
	var buf []lineEntry

	// Flusher goroutine
	go func() {
		ticker := time.NewTicker(flushInterval)
		defer ticker.Stop()
		for range ticker.C {
			mu.Lock()
			if len(buf) == 0 {
				mu.Unlock()
				continue
			}
			batch := buf
			buf = nil
			mu.Unlock()
			send(batch)
		}
	}()

	for {
		conn, err := ln.Accept()
		if err != nil {
			log.Printf("accept: %v", err)
			continue
		}
		go handle(conn, &mu, &buf)
	}
}

func handle(conn net.Conn, mu *sync.Mutex, buf *[]lineEntry) {
	defer conn.Close()
	host, _, _ := net.SplitHostPort(conn.RemoteAddr().String())
	sc := bufio.NewScanner(conn)
	for sc.Scan() {
		line := sc.Text()
		if line == "" {
			continue
		}
		mu.Lock()
		*buf = append(*buf, lineEntry{line: line, time: time.Now()})
		if len(*buf) >= flushMaxLines {
			batch := *buf
			*buf = nil
			mu.Unlock()
			send(batch)
		} else {
			mu.Unlock()
		}
	}
	log.Printf("[%s] disconnected", host)
}

func send(batch []lineEntry) {
	entries := make([][2]string, len(batch))
	for i, e := range batch {
		ns := e.time.UnixNano()
		// Loki expects timestamp as string in nanoseconds (or other formats)
		entries[i] = [2]string{fmt.Sprintf("%d", ns), e.line}
	}

	// Try to extract host from the first entry for labeling
	host := deriveHost(batch)

	body, _ := json.Marshal(pushReq{
		Streams: []stream{{
			Stream: map[string]string{
				"job":    "gtnh",
				"host":   host,
				"source": "tcp-bridge",
			},
			Values: entries,
		}},
	})

	req, err := http.NewRequest("POST", *lokiURL, bytes.NewReader(body))
	if err != nil {
		log.Printf("http req: %v", err)
		return
	}
	req.Header.Set("Content-Type", "application/json")

	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		log.Printf("push to loki: %v", err)
		return
	}
	resp.Body.Close()
	if resp.StatusCode >= 300 {
		log.Printf("loki returned %d for %d lines", resp.StatusCode, len(batch))
	}
}

// deriveHost returns host label from the batch or falls back to flag.
var hostGuess string

func deriveHost(batch []lineEntry) string {
	if hostGuess != "" {
		return hostGuess
	}
	if len(batch) > 0 {
		hostGuess = os.Getenv("HOSTNAME")
	}
	if hostGuess == "" {
		hostGuess = *hostLabel
	}
	return hostGuess
}
