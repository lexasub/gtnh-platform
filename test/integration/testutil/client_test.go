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

// TestBuildPlaceBlockActionCarriesHeldItem pins the placement contract: the
// server claims a placement only when held_item != 0
// (PlaceBlockHandler::canHandle, PlaceBlockHandler.cpp:14-18), and the
// production client always writes the equipped item (NetClient.cpp:689-706).
// A builder that silently dropped held_item would make every placement test
// fail with a confusing REJECTED instead of an obvious encode error.
func TestBuildPlaceBlockActionCarriesHeldItem(t *testing.T) {
	const blockID uint16 = 0xE000
	action := Protocol.GetRootAsSetBlockAction(
		BuildPlaceBlockAction(1, 2, 3, 4, 0, blockID), 0)
	if action.HeldItem() != blockID {
		t.Fatalf("held_item = %d, want %d", action.HeldItem(), blockID)
	}
	if action.NewBlockId() != blockID {
		t.Fatalf("new_block_id = %d, want %d", action.NewBlockId(), blockID)
	}
	if action.Action() != Protocol.PlayerActionTypeRIGHT_MOUSE_CLICK {
		t.Fatalf("action = %v, want RIGHT_MOUSE_CLICK", action.Action())
	}
	// Face 0 (DOWN) is what makes the server's --y land on the intended cell.
	if action.Face() != 0 {
		t.Fatalf("face = %d, want 0 (DOWN)", action.Face())
	}

	withOpts := Protocol.GetRootAsSetBlockAction(
		BuildPlaceBlockActionWithOptions(1, 2, 3, 4, 0, blockID,
			SetBlockActionOptions{RequestID: 55, Face: 2}), 0)
	if withOpts.RequestId() != 55 || withOpts.Face() != 2 {
		t.Fatalf("options not encoded: request=%d face=%d", withOpts.RequestId(), withOpts.Face())
	}
	// held_item is always the placed block, never whatever the caller passed.
	if withOpts.HeldItem() != blockID {
		t.Fatalf("held_item = %d, want %d", withOpts.HeldItem(), blockID)
	}
}

// TestBuildSetBlockActionIsEmptyHand documents the deliberate difference: the
// no-options builder produces an empty hand, which the server rejects. Tests
// that mean to place a block must use BuildPlaceBlockAction.
func TestBuildSetBlockActionIsEmptyHand(t *testing.T) {
	action := Protocol.GetRootAsSetBlockAction(BuildSetBlockAction(1, 2, 3, 4, 0, 7), 0)
	if action.HeldItem() != 0 {
		t.Fatalf("held_item = %d, want 0 (empty hand)", action.HeldItem())
	}
	if action.NewBlockId() != 7 {
		t.Fatalf("new_block_id = %d, want 7", action.NewBlockId())
	}
}

func TestInventoryItemCountSumsAcrossSlots(t *testing.T) {
	data := buildTestInventoryUpdate(1, []testSlot{
		{itemID: 100, count: 12},
		{itemID: 22530, count: 4},
		{itemID: 22530, count: 3},
	})
	if got := InventoryItemCount(data, 22530); got != 7 {
		t.Fatalf("InventoryItemCount = %d, want 7", got)
	}
	if got := InventoryItemCount(data, 999); got != 0 {
		t.Fatalf("InventoryItemCount for an absent item = %d, want 0", got)
	}
	// A zero-length payload has no root table; the helper must not panic.
	if got := InventoryItemCount(nil, 22530); got != 0 {
		t.Fatalf("InventoryItemCount(nil) = %d, want 0", got)
	}
	if got := InventoryItemCount([]byte{}, 22530); got != 0 {
		t.Fatalf("InventoryItemCount(empty) = %d, want 0", got)
	}
}

func TestFirstSlotWithItemFindsRealSlot(t *testing.T) {
	data := buildTestInventoryUpdate(1, []testSlot{
		{itemID: 100, count: 1},
		{itemID: 0, count: 0},
		{itemID: 22530, count: 4},
		{itemID: 22530, count: 2},
	})
	if got := FirstSlotWithItem(data, 22530); got != 2 {
		t.Fatalf("FirstSlotWithItem = %d, want 2", got)
	}
	if got := FirstSlotWithItem(data, 777); got != -1 {
		t.Fatalf("FirstSlotWithItem for an absent item = %d, want -1", got)
	}
	if got := FirstSlotWithItem(nil, 22530); got != -1 {
		t.Fatalf("FirstSlotWithItem(nil) = %d, want -1", got)
	}
}

// TestWaitForCursorItemClosesTheGapLeftByWaitForInventoryItem pins the
// predicate this package was missing: the server-owned cursor
// (InventoryClick.h:148-153 moves a picked-up stack OUT of the player grid and
// onto it) is a field of the same InventoryUpdate frame, but nothing could
// wait on it.
//
// WaitForInventoryItem can only assert POSITIVE inventory state. A click test
// that has to prove "the stack left the grid" therefore asserts the absence of
// an item, which any of three unrelated events can also produce: another test
// in the same process picking the item up, the item sitting on the cursor, or
// the server splitting it across slots. This helper gives that test a
// POSITIVE assertion instead.
func TestWaitForCursorItemClosesTheGapLeftByWaitForInventoryItem(t *testing.T) {
	client, server := pipeClient(t)

	// Every frame below must be skipped: wrong type, empty payload, another
	// player, an empty cursor, and a cursor holding a different item. The last
	// frame is the one that satisfies the predicate.
	writer := pushCtrlFrames(server, []ctrlFrame{
		{msgType: MsgGameModeChange, payload: nil},
		{msgType: MsgInventoryUpdate, payload: nil},
		{msgType: MsgInventoryUpdate, payload: buildTestInventoryUpdate(7, []testSlot{})},
		{msgType: MsgInventoryUpdate, payload: buildTestInventoryUpdateWithCursor(1, 22531, 9, []testSlot{{itemID: 22530, count: 4}})},
		{msgType: MsgInventoryUpdate, payload: buildTestInventoryUpdateWithCursor(1, 22530, 4, []testSlot{})},
	})

	matched, err := client.WaitForCursorItem(1, 22530, 4, 2*time.Second)
	if err != nil {
		t.Fatalf("WaitForCursorItem: %v", err)
	}
	var stack Protocol.ItemStack
	// Cursor() returns a zero-valued struct rather than nil when the frame
	// carries no cursor, so read the fields rather than testing for nil.
	Protocol.GetRootAsInventoryUpdate(matched, 0).Cursor(&stack)
	if stack.ItemId() != 22530 || stack.Count() != 4 {
		t.Fatalf("cursor = item %d x%d, want item 22530 x4", stack.ItemId(), stack.Count())
	}
	awaitWriter(t, writer)
}

// TestWaitForCursorItemRespectsMinCount is the split-stack guard: a pick-up
// click moves the WHOLE stack, but a partial move leaves fewer items on the
// cursor than the grid had. A helper that ignored minCount would accept the
// partial state and the test would pass without the click having happened.
func TestWaitForCursorItemRespectsMinCount(t *testing.T) {
	client, server := pipeClient(t)

	writer := pushCtrlFrames(server, []ctrlFrame{
		{msgType: MsgInventoryUpdate, payload: buildTestInventoryUpdateWithCursor(1, 22530, 2, []testSlot{})},
	})

	_, err := client.WaitForCursorItem(1, 22530, 4, 300*time.Millisecond)
	if err == nil {
		t.Fatal("WaitForCursorItem accepted a cursor holding fewer items than minCount")
	}
	if !strings.Contains(err.Error(), "timeout waiting for cursor") {
		t.Fatalf("WaitForCursorItem error = %q, want deadline timeout error", err)
	}
	awaitWriter(t, writer)
}

// TestWaitForCursorItemTimesOutWhenTheStackStaysInTheGrid is the RED for the
// defect this helper exists to remove: the hand-rolled loop in
// TestInventory_MoveBetweenSlots asserted only that the item was GONE from the
// player grid. A server that silently dropped the pick-up — publishing
// nothing, or moving the stack to a slot the snapshot omits — satisfies that
// negative assertion. This asserts the positive fact instead, and fails when
// the stack is still in the grid.
func TestWaitForCursorItemTimesOutWhenTheStackStaysInTheGrid(t *testing.T) {
	client, server := pipeClient(t)

	// The grid still holds all 4 and the cursor is empty: the click did NOT
	// move anything. A negative "item not in grid" check would not fire here
	// (the item IS in the grid), but neither would it catch a silent drop
	// where the item vanishes from BOTH — the cursor predicate does.
	writer := pushCtrlFrames(server, []ctrlFrame{
		{msgType: MsgInventoryUpdate, payload: buildTestInventoryUpdateWithCursor(1, 0, 0, []testSlot{{itemID: 22530, count: 4}})},
	})

	if _, err := client.WaitForCursorItem(1, 22530, 4, 300*time.Millisecond); err == nil {
		t.Fatal("WaitForCursorItem succeeded while the stack was still in the player grid")
	}
	awaitWriter(t, writer)
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

// buildTestInventoryUpdateWithCursor is buildTestInventoryUpdate plus the
// server-owned cursor struct, which PlayerInventoryStore::buildUpdate always
// fills (PlayerInventoryStore.cpp:185-196) and which a pick-up click is the
// only way to populate.
func buildTestInventoryUpdateWithCursor(playerID uint64, cursorID uint16, cursorCount byte, slots []testSlot) []byte {
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
	// The cursor is an inline struct: CreateItemStack runs INSIDE the table's
	// Start/End block and its offset is handed to AddCursor — the same shape
	// CreateVec3i/AddPos uses in the production builders. Creating it before
	// Start panics in PrependStructSlot.
	cursorOffset := Protocol.CreateItemStack(b, cursorID, cursorCount, 0)
	Protocol.InventoryUpdateAddCursor(b, cursorOffset)
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
