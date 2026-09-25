package main

import (
	"bytes"
	"io"
	"net"
	"testing"
)

func TestReadRouterFrameQueuedPayloadsDoNotAliasReceiveBuffer(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()
	defer server.Close()

	firstPayload := []byte("first queued payload")
	secondPayload := []byte("second queued payload")
	go func() {
		for _, payload := range [][]byte{firstPayload, secondPayload} {
			if err := writeFrame(server, msgPublish, payload); err != nil {
				return
			}
		}
	}()

	buf := make([]byte, 64*1024)
	firstType, first, err := readRouterFrame(client, buf)
	if err != nil {
		t.Fatalf("read first frame: %v", err)
	}
	secondType, second, err := readRouterFrame(client, buf)
	if err != nil {
		t.Fatalf("read second frame: %v", err)
	}

	if firstType != msgPublish || secondType != msgPublish {
		t.Fatalf("frame types = (%d, %d), want (%d, %d)", firstType, secondType, msgPublish, msgPublish)
	}
	if &first[0] == &second[0] {
		t.Fatal("queued payloads alias the reusable receive buffer")
	}
	if !bytes.Equal(first, firstPayload) {
		t.Fatalf("first payload = %q, want %q", first, firstPayload)
	}
	if !bytes.Equal(second, secondPayload) {
		t.Fatalf("second payload = %q, want %q", second, secondPayload)
	}
}

func TestReadRouterFrameRejectsShortLength(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()
	defer server.Close()

	go func() {
		_, _ = server.Write([]byte{0, 0, 0, 0, byte(msgPublish)})
	}()

	_, _, err := readRouterFrame(client, make([]byte, 64*1024))
	if err != io.ErrUnexpectedEOF {
		t.Fatalf("error = %v, want %v", err, io.ErrUnexpectedEOF)
	}
}
