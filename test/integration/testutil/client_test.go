package testutil

import (
	"encoding/binary"
	"net"
	"strings"
	"testing"
	"time"

	flatbuffers "github.com/google/flatbuffers/go"
	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
)

func TestDecodeMultiblockEventRejectsEmpty(t *testing.T) {
	if _, _, _, err := DecodeMultiblockEvent(nil); err == nil {
		t.Fatal("expected empty event error")
	}
}

func TestBuildSetBlockActionWithOptionsCarriesRequestID(t *testing.T) {
	data := BuildSetBlockActionWithOptions(1, 2, 3, 4, 0, 1001, SetBlockActionOptions{RequestID: 77, Face: 5, HeldItem: 1001})
	action := Protocol.GetRootAsSetBlockAction(data, 0)
	if action.RequestId() != 77 || action.Face() != 5 || action.HeldItem() != 1001 {
		t.Fatalf("options not encoded: request=%d face=%d held=%d", action.RequestId(), action.Face(), action.HeldItem())
	}
}

type testSlot struct {
	itemID uint16
	count  byte
}

// buildTestInventoryUpdate builds a minimal InventoryUpdate snapshot so the
// predicate wait can be exercised without starting any service.
func buildTestInventoryUpdate(playerID uint64, slots []testSlot) []byte {
	b := flatbuffers.NewBuilder(256)
	offsets := make([]flatbuffers.UOffsetT, len(slots))
	for i, slot := range slots {
		Protocol.InventorySlotStart(b)
		Protocol.InventorySlotAddItemId(b, slot.itemID)
		Protocol.InventorySlotAddCount(b, slot.count)
		offsets[i] = Protocol.InventorySlotEnd(b)
	}
	Protocol.InventoryUpdateStartSlotsVector(b, len(offsets))
	for i := len(offsets) - 1; i >= 0; i-- {
		b.PrependUOffsetT(offsets[i])
	}
	slotsVector := b.EndVector(len(offsets))
	Protocol.InventoryUpdateStart(b)
	Protocol.InventoryUpdateAddPlayerId(b, playerID)
	Protocol.InventoryUpdateAddSlots(b, slotsVector)
	b.Finish(Protocol.InventoryUpdateEnd(b))
	return b.FinishedBytes()
}

// pipeClient wires a GatewayClient to an in-memory server end and returns both
// so a test can push exact frames the client will read.
func pipeClient(t *testing.T) (*GatewayClient, net.Conn) {
	t.Helper()
	clientConn, serverConn := net.Pipe()
	t.Cleanup(func() {
		serverConn.Close()
		clientConn.Close()
	})
	return &GatewayClient{conn: clientConn}, serverConn
}

type ctrlFrame struct {
	msgType uint8
	payload []byte
}

func ctrlFrameBytes(msgType uint8, payload []byte) []byte {
	frame := make([]byte, 5+len(payload))
	binary.BigEndian.PutUint32(frame[0:4], uint32(1+len(payload)))
	frame[4] = msgType
	copy(frame[5:], payload)
	return frame
}

// pushCtrlFrames writes frames on a background goroutine and waits for the
// writer to finish. A client that stops reading leaves the writer blocked on
// the pipe, so closing the connection is the only way to release it; errors are
// reported through the returned channel instead of t.Fatalf, which is not
// allowed off the test goroutine.
func pushCtrlFrames(conn net.Conn, frames []ctrlFrame) <-chan error {
	done := make(chan error, 1)
	go func() {
		for _, frame := range frames {
			if _, err := conn.Write(ctrlFrameBytes(frame.msgType, frame.payload)); err != nil {
				done <- err
				return
			}
		}
		done <- nil
	}()
	return done
}

func awaitWriter(t *testing.T, done <-chan error) {
	t.Helper()
	select {
	case err := <-done:
		if err != nil {
			// The test may finish before every queued frame is consumed; a
			// closed pipe here is expected and not a failure of the assertion.
			t.Logf("frame writer stopped early: %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Log("frame writer still blocked; connection close will release it")
	}
}

func TestWaitForInventoryItemSkipsStaleSnapshots(t *testing.T) {
	client, server := pipeClient(t)

	// Every frame below must be skipped: wrong message type, empty payload,
	// another player, a different item, and finally a matching snapshot whose
	// item count is split across two slots.
	writer := pushCtrlFrames(server, []ctrlFrame{
		{msgType: MsgGameModeChange, payload: nil},
		{msgType: MsgInventoryUpdate, payload: nil},
		{msgType: MsgInventoryUpdate, payload: buildTestInventoryUpdate(7, []testSlot{{itemID: 22530, count: 8}})},
		{msgType: MsgInventoryUpdate, payload: buildTestInventoryUpdate(1, []testSlot{{itemID: 22531, count: 64}})},
		{msgType: MsgInventoryUpdate, payload: buildTestInventoryUpdate(1, []testSlot{{itemID: 22530, count: 2}})},
		{msgType: MsgInventoryUpdate, payload: buildTestInventoryUpdate(1, []testSlot{
			{itemID: 100, count: 12},
			{itemID: 22530, count: 4},
			{itemID: 22530, count: 4},
		})},
	})

	matched, err := client.WaitForInventoryItem(1, 22530, 8, 2*time.Second)
	if err != nil {
		t.Fatalf("WaitForInventoryItem: %v", err)
	}
	update := Protocol.GetRootAsInventoryUpdate(matched, 0)
	if update.PlayerId() != 1 {
		t.Fatalf("matched snapshot player = %d, want 1", update.PlayerId())
	}
	awaitWriter(t, writer)
}

func TestWaitForInventoryItemSumsSplitStacks(t *testing.T) {
	client, server := pipeClient(t)

	writer := pushCtrlFrames(server, []ctrlFrame{
		{msgType: MsgInventoryUpdate, payload: buildTestInventoryUpdate(1, []testSlot{
			{itemID: 22530, count: 4},
			{itemID: 22530, count: 4},
		})},
	})

	if _, err := client.WaitForInventoryItem(1, 22530, 8, 2*time.Second); err != nil {
		t.Fatalf("WaitForInventoryItem rejected a split stack: %v", err)
	}
	awaitWriter(t, writer)
}

func TestWaitForInventoryItemTimesOutOnUnmatchedSnapshots(t *testing.T) {
	client, server := pipeClient(t)

	writer := pushCtrlFrames(server, []ctrlFrame{
		{msgType: MsgInventoryUpdate, payload: buildTestInventoryUpdate(1, []testSlot{{itemID: 22530, count: 1}})},
		{msgType: MsgInventoryUpdate, payload: buildTestInventoryUpdate(1, []testSlot{{itemID: 22531, count: 64}})},
	})

	_, err := client.WaitForInventoryItem(1, 22530, 8, 300*time.Millisecond)
	if err == nil {
		t.Fatal("WaitForInventoryItem returned nil error, want timeout error")
	}
	// The wait must end on its own deadline, not on an incidental read error.
	if !strings.Contains(err.Error(), "timeout waiting for inventory") {
		t.Fatalf("WaitForInventoryItem error = %q, want deadline timeout error", err)
	}
	awaitWriter(t, writer)
}
