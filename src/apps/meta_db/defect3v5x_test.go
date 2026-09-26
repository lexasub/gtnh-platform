// gp-3v5x: regression tests for the two MetaDB boundary defects plus the
// same defect class in the sidecar handlers.
//
// DEFECT 1: the "logout" JSON path built one FlatBuffer per slot and then
// re-parsed it with GetRootAsInventorySlot(builder.FinishedBytes(), off) —
// FinishedBytes without Finish(), which panics unconditionally in
// flatbuffers/go builder.go:74-77 (assertFinished, builder.go:424). The path
// panicked for ANY non-empty slot list, so these tests drive the real
// handleRequest entry point rather than a synthetic reproduction.
//
// DEFECT 2: handleFlatBufferConnection parsed attacker-controlled frames on a
// bare goroutine with no recover(), so one malformed frame killed metadbd.
// The tests below drive the real handler over a real net.Pipe.
package main

import (
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"path/filepath"
	"testing"
	"time"

	flatbuffers "github.com/google/flatbuffers/go"

	"github.com/gtnh-platform/protocol/generated/go/Protocol"
)

func newTestMetaDB(t *testing.T) *MetaDB {
	t.Helper()
	m, err := NewMetaDB(filepath.Join(t.TempDir(), "gp-3v5x.db"))
	if err != nil {
		t.Fatalf("NewMetaDB: %v", err)
	}
	t.Cleanup(func() { m.db.Close() })
	return m
}

// writeFBFrame writes one [4-byte BE length][payload] frame, the exact framing
// handleFlatBufferConnection reads.
func writeFBFrame(c net.Conn, payload []byte) error {
	hdr := make([]byte, 4)
	binary.BigEndian.PutUint32(hdr, uint32(len(payload)))
	if _, err := c.Write(hdr); err != nil {
		return err
	}
	_, err := c.Write(payload)
	return err
}

// ---------------------------------------------------------------------------
// DEFECT 1 — FinishedBytes without Finish() on the inventory-save path
// ---------------------------------------------------------------------------

func TestLogoutWithNonEmptySlotsDoesNotPanic(t *testing.T) {
	m := newTestMetaDB(t)

	req := Request{
		Action:   "logout",
		PlayerID: 42,
		Data: map[string]interface{}{
			"slots": []interface{}{
				map[string]interface{}{"slot": float64(0), "block_id": float64(1234), "count": float64(5)},
				map[string]interface{}{"slot": float64(1), "block_id": float64(5678), "count": float64(9)},
			},
			"x": float64(10),
			"y": float64(64),
			"z": float64(-3),
		},
	}

	// Pre-fix this call panics with:
	//   "If you get this assert, you're attempting to get access a buffer
	//    which hasn't been finished yet. Be sure to call builder.Finish()..."
	// on the very first slot, and the test binary dies.
	resp := handleRequest(m, req)
	if !resp.Success {
		t.Fatalf("logout failed: %v", resp.Error)
	}

	// The save must actually have landed: this path exists to persist slots.
	slots, err := m.GetInventory(42)
	if err != nil {
		t.Fatalf("GetInventory: %v", err)
	}
	if len(slots) != 2 {
		t.Fatalf("saved %d slots, want 2", len(slots))
	}
	if slots[0].BlockID != 1234 || slots[0].Count != 5 {
		t.Fatalf("slot 0 = %+v, want block_id=1234 count=5", slots[0])
	}
	if slots[1].BlockID != 5678 || slots[1].Count != 9 {
		t.Fatalf("slot 1 = %+v, want block_id=5678 count=9", slots[1])
	}

	pos, err := m.GetPlayerPosition(42)
	if err != nil {
		t.Fatalf("GetPlayerPosition: %v", err)
	}
	if pos.X != 10 || pos.Y != 64 || pos.Z != -3 {
		t.Fatalf("position = %+v, want [10 64 -3]", pos)
	}
}

// A single slot is enough to trip the un-Finished builder; guard that the
// minimal case works too.
func TestLogoutWithSingleSlotDoesNotPanic(t *testing.T) {
	m := newTestMetaDB(t)

	resp := handleRequest(m, Request{
		Action:   "logout",
		PlayerID: 7,
		Data: map[string]interface{}{
			"slots": []interface{}{
				map[string]interface{}{"slot": float64(3), "block_id": float64(99), "count": float64(1)},
			},
		},
	})
	if !resp.Success {
		t.Fatalf("logout failed: %v", resp.Error)
	}
	slots, err := m.GetInventory(7)
	if err != nil {
		t.Fatalf("GetInventory: %v", err)
	}
	if len(slots) != 1 || slots[0].BlockID != 99 {
		t.Fatalf("slots = %+v, want one slot with block_id=99", slots)
	}
}

// ---------------------------------------------------------------------------
// DEFECT 2 — malformed frame must not take the process / connection down
// ---------------------------------------------------------------------------

// malformedFBFrames are buffers that GetRootAsMetaDBFrame accepts (it never
// returns nil) but whose accessors then index out of range. The first entry is
// the audit's live probe: "index out of range [3] with length 0".
var malformedFBFrames = [][]byte{
	{0x00, 0x00, 0x00, 0x00},                         // 4 zero bytes
	{0x04, 0x00, 0x00, 0x00},                         // root offset past EOF
	{0x0c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00}, // vtable offset 1 past start
	{0xff, 0xff, 0xff, 0xff},                         // root offset wraps
	{0x00, 0x00},                                     // 2 bytes
	{0x10, 0x00, 0x00, 0x00, 0xde, 0xad, 0xbe, 0xef, 0x01, 0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02},
}

// TestMalformedFBFrameDoesNotPanicOnConnectionGoroutine drives the real
// handleFlatBufferConnection. Pre-fix the handler goroutine panics, which
// takes the whole process with it — the test binary dies rather than failing,
// which is exactly the remote DoS. Post-fix the handler must return cleanly.
func TestMalformedFBFrameDoesNotPanicOnConnectionGoroutine(t *testing.T) {
	for i, bad := range malformedFBFrames {
		func() {
			m := newTestMetaDB(t)
			client, server := net.Pipe()
			defer client.Close()
			defer server.Close()

			done := make(chan struct{})
			go func() {
				defer close(done)
				// Before the fix this goroutine has no recover(); a panic here
				// crashes the process.
				handleFlatBufferConnection(server, m)
			}()

			if err := writeFBFrame(client, bad); err != nil {
				t.Fatalf("frame %d: write: %v", i, err)
			}
			// The handler must survive and return (client Close ends the loop).
			client.Close()

			select {
			case <-done:
			case <-time.After(10 * time.Second):
				t.Fatalf("frame %d: handler did not return; recover() is missing or the handler is stuck", i)
			}
		}()
	}
}

// A malformed frame must be DROPPED, not answered, and the connection must stay
// usable: the verifier rejects before any accessor runs, so the stream is still
// in sync and a subsequent well-formed frame is served normally. This is the
// "verifier rejection" path of the close-vs-continue decision.
func TestMalformedFBFrameIsDroppedAndConnectionSurvives(t *testing.T) {
	m := newTestMetaDB(t)
	// Give the player a saved inventory so the good frame has something to return.
	if err := m.UpdateInventorySlot(11, 0, 4321, 3); err != nil {
		t.Fatalf("seed inventory: %v", err)
	}

	client, server := net.Pipe()
	defer client.Close()
	defer server.Close()

	done := make(chan struct{})
	go func() {
		defer close(done)
		handleFlatBufferConnection(server, m)
	}()

	// Frame 1: garbage, which the verifier must reject silently.
	if err := writeFBFrame(client, malformedFBFrames[1]); err != nil {
		t.Fatalf("write malformed: %v", err)
	}
	// Frame 2: well-formed GetInventoryReq on the SAME connection.
	if err := writeFBFrame(client, buildGetInventoryReqFrame(t, 11)); err != nil {
		t.Fatalf("write good: %v", err)
	}

	// The good frame must be answered: that proves the connection survived the
	// malformed frame and the handler is still serving.
	client.SetReadDeadline(time.Now().Add(10 * time.Second))
	var lenBuf [4]byte
	if _, err := io.ReadFull(client, lenBuf[:]); err != nil {
		t.Fatalf("connection died on a malformed frame instead of dropping it: %v", err)
	}
	n := binary.BigEndian.Uint32(lenBuf[:])
	if n == 0 || n > 1<<20 {
		t.Fatalf("reply length = %d, want a real response", n)
	}
	resp := make([]byte, n)
	if _, err := io.ReadFull(client, resp); err != nil {
		t.Fatalf("read reply body: %v", err)
	}
	if _, err := fbVerifyFrame(resp); err != nil {
		t.Fatalf("reply is not a well-formed MetaDBFrame: %v", err)
	}

	client.Close()
	select {
	case <-done:
	case <-time.After(10 * time.Second):
		t.Fatal("handler goroutine did not return after the client closed")
	}
}

// TestHandleFlatBufferConnectionRecoversFromPanic proves the recover() backstop
// is really wired in, not just the verifier. The verifier covers every table in
// fbverify.go, so a panic can only be provoked by dispatching a frame the
// verifier does not model — which is exactly the "a new table was added and
// nobody extended the verifier" case the backstop exists for. Here the panic is
// injected through the frame-dispatch seam to prove the recover fires and the
// process survives.
func TestHandleFlatBufferConnectionRecoversFromPanic(t *testing.T) {
	m := newTestMetaDB(t)
	client, server := net.Pipe()
	defer client.Close()
	defer server.Close()

	done := make(chan struct{})
	go func() {
		defer close(done)
		// No recover() of our own here: if handleFlatBufferConnection lacked its
		// own, this goroutine's panic would take down the whole test binary,
		// which is precisely the pre-fix remote DoS.
		handleFlatBufferConnectionWith(server, m, fbPanicDispatch)
	}()

	// A structurally VALID MetaDBFrame: verification passes, then the injected
	// dispatch seam panics, exercising the backstop.
	if err := writeFBFrame(client, buildGetInventoryReqFrame(t, 5)); err != nil {
		t.Fatalf("write: %v", err)
	}

	select {
	case <-done:
		// Recovered: the handler returned normally and the process is alive.
	case <-time.After(10 * time.Second):
		t.Fatal("handler still running after the injected panic: no recover() at the connection boundary")
	}
}

// TestHandleFlatBufferConnectionRejectsWithoutPanic feeds a malformed frame
// directly and asserts the handler returns instead of panicking.
func TestHandleFlatBufferConnectionRejectsWithoutPanic(t *testing.T) {
	m := newTestMetaDB(t)
	client, server := net.Pipe()
	defer client.Close()
	defer server.Close()

	errCh := make(chan struct{}, 1)
	go func() {
		defer func() {
			if r := recover(); r != nil {
				errCh <- struct{}{}
				panic(fmt.Sprintf("handleFlatBufferConnection panicked: %v", r))
			}
			close(errCh)
		}()
		handleFlatBufferConnection(server, m)
	}()

	if err := writeFBFrame(client, []byte{0x00, 0x00, 0x00, 0x00}); err != nil {
		t.Fatalf("write: %v", err)
	}
	client.Close()

	select {
	case _, ok := <-errCh:
		if !ok {
			return // clean return
		}
		t.Fatal("handleFlatBufferConnection panicked on a malformed frame")
	case <-time.After(10 * time.Second):
		t.Fatal("handleFlatBufferConnection did not return")
	}
}

// fbPanicDispatch is a dispatch seam that panics on every frame. It stands in
// for the case the verifier cannot cover by itself: a frame that is
// structurally valid but that some later code mishandles. Pre-fix there was no
// recover() at the connection boundary, so this panic propagated out of the
// goroutine and took the process with it; post-fix it is contained and logged.
func fbPanicDispatch(frame *Protocol.MetaDBFrame, m *MetaDB) []byte {
	panic("injected dispatch panic (fault injection for the recover() backstop)")
}

// ---------------------------------------------------------------------------
// DEFECT 2, second half — the two unguarded PlayerJoined / PlayerLeft handlers
//
// Both had no validation at all. GetRootAsPlayerJoined on a 4-zero-byte
// payload returned a table reading player_id = 0, so handlePlayerJoined went
// on to publish player 0's inventory and position, and handlePlayerLeft wrote
// [0,0,0] over player 0's saved position. These assert the wrong-player write
// no longer happens.
// ---------------------------------------------------------------------------

// TestHandlePlayerLeftRejectsMalformedPayload is the wrong-player-WRITE test:
// player 0 has a real saved position, and a malformed player.left payload must
// not overwrite it.
func TestHandlePlayerLeftRejectsMalformedPayload(t *testing.T) {
	m := newTestMetaDB(t)
	if err := m.SavePlayerPosition(0, 111, 222, 333); err != nil {
		t.Fatalf("seed position: %v", err)
	}

	for i, bad := range malformedFBFrames {
		func() {
			// Must not panic and must not touch the database.
			handlePlayerLeft(bad, m)
		}()
		if pos, err := m.GetPlayerPosition(0); err != nil {
			t.Fatalf("frame %d: position row destroyed: %v", i, err)
		} else if pos.X != 111 || pos.Y != 222 || pos.Z != 333 {
			t.Fatalf("frame %d (% x): malformed payload overwrote player 0 position: %+v", i, bad, pos)
		}
	}
}

// TestHandlePlayerJoinedRejectsMalformedPayload is the wrong-player-READ test.
// Player 0 has a saved inventory and a saved position; a malformed
// player.joined payload must be rejected BEFORE any of that is read, so the
// only observable proof is the rejection itself — assert it directly rather
// than through a side effect.
func TestHandlePlayerJoinedRejectsMalformedPayload(t *testing.T) {
	m := newTestMetaDB(t)
	if err := m.UpdateInventorySlot(0, 0, 7777, 7); err != nil {
		t.Fatalf("seed inventory: %v", err)
	}

	for i, bad := range malformedFBFrames {
		// Must not panic, and must not accept the payload.
		if _, err := fbVerifiedPlayerJoined(bad); err == nil {
			t.Fatalf("frame %d (% x): verifier accepted a malformed player.joined payload", i, bad)
		}
		// The handler must not have created or mutated any player-0 state.
		// handlePlayerJoined only READS on this path (publish is a no-op with
		// m.rc == nil), so the seeded inventory must be untouched.
		slots, err := m.GetInventory(0)
		if err != nil {
			t.Fatalf("frame %d: inventory query failed: %v", i, err)
		}
		if len(slots) != 1 || slots[0].BlockID != 7777 {
			t.Fatalf("frame %d (% x): malformed payload disturbed player 0 state: %+v", i, bad, slots)
		}
		// The handler itself must return rather than panic.
		handlePlayerJoined(bad, m)
	}
}

// TestHandlePlayerLeftAcceptsValidPayload is the control: a well-formed
// PlayerLeft must still be applied, so the guard is not over-rejecting.
func TestHandlePlayerLeftAcceptsValidPayload(t *testing.T) {
	m := newTestMetaDB(t)

	b := flatbuffers.NewBuilder(64)
	Protocol.PlayerLeftStart(b)
	Protocol.PlayerLeftAddPlayerId(b, 4242)
	Protocol.PlayerLeftAddX(b, 10)
	Protocol.PlayerLeftAddY(b, -64)
	Protocol.PlayerLeftAddZ(b, 300)
	off := Protocol.PlayerLeftEnd(b)
	b.Finish(off)

	handlePlayerLeft(b.FinishedBytes(), m)

	pos, err := m.GetPlayerPosition(4242)
	if err != nil {
		t.Fatalf("valid PlayerLeft was rejected: %v", err)
	}
	if pos.X != 10 || pos.Y != -64 || pos.Z != 300 {
		t.Fatalf("position = %+v, want [10 -64 300]", pos)
	}
}

// TestHandlePlayerJoinedAcceptsValidPayload is the control for the join side.
func TestHandlePlayerJoinedAcceptsValidPayload(t *testing.T) {
	m := newTestMetaDB(t)
	if err := m.SavePlayerPosition(5150, 1, 2, 3); err != nil {
		t.Fatalf("seed position: %v", err)
	}

	b := flatbuffers.NewBuilder(32)
	Protocol.PlayerJoinedStart(b)
	Protocol.PlayerJoinedAddPlayerId(b, 5150)
	off := Protocol.PlayerJoinedEnd(b)
	b.Finish(off)

	handlePlayerJoined(b.FinishedBytes(), m) // m.rc == nil, publish is a no-op

	pos, err := m.GetPlayerPosition(5150)
	if err != nil {
		t.Fatalf("valid PlayerJoined was rejected: %v", err)
	}
	if pos.X != 1 || pos.Y != 2 || pos.Z != 3 {
		t.Fatalf("position = %+v, want [1 2 3]", pos)
	}
}

// ---------------------------------------------------------------------------
// The verifier itself
// ---------------------------------------------------------------------------

// TestVerifyRootRejectsEveryMalformedFrame is the unit-level backstop: every
// probe buffer must be rejected, and every table the handlers parse must be
// covered by a descriptor.
func TestVerifyRootRejectsEveryMalformedFrame(t *testing.T) {
	descs := map[string]*fbTable{
		"MetaDBFrame":           fbMetaDBFrame,
		"MetaDBMessage":         fbMetaDBMessage,
		"GetInventoryReq":       fbGetInventoryReq,
		"SetInventorySlotReq":   fbSetInventorySlotReq,
		"GetInventorySnapshot":  fbGetInventorySnapshotReq,
		"QuestProgressUpdate":   fbQuestProgressUpdate,
		"QuestCompleted":        fbQuestCompleted,
		"QuestExchangeRequest":  fbQuestExchangeRequest,
		"QuestExchangeCooldown": fbQuestExchangeCooldownGet,
		"PlayerJoined":          fbPlayerJoined,
		"PlayerLeft":            fbPlayerLeft,
	}
	for name, desc := range descs {
		for i, bad := range malformedFBFrames {
			if _, err := verifyRoot(bad, desc); err == nil {
				t.Fatalf("%s: verifier ACCEPTED malformed buffer %d (% x) — it must reject", name, i, bad)
			}
		}
	}
}

// TestVerifyRootAcceptsValidBuffers is the control: the verifier must not
// reject well-formed frames, or the service would drop every real request.
func TestVerifyRootAcceptsValidBuffers(t *testing.T) {
	frame := buildGetInventoryReqFrame(t, 99)
	if _, err := fbVerifyFrame(frame); err != nil {
		t.Fatalf("verifier rejected a valid MetaDBFrame: %v", err)
	}

	// A PlayerLeft with every field present.
	b := flatbuffers.NewBuilder(64)
	Protocol.PlayerLeftStart(b)
	Protocol.PlayerLeftAddPlayerId(b, 1)
	Protocol.PlayerLeftAddX(b, 2)
	Protocol.PlayerLeftAddY(b, 3)
	Protocol.PlayerLeftAddZ(b, 4)
	off := Protocol.PlayerLeftEnd(b)
	b.Finish(off)
	if _, err := fbVerifiedPlayerLeft(b.FinishedBytes()); err != nil {
		t.Fatalf("verifier rejected a valid PlayerLeft: %v", err)
	}

	// An EMPTY PlayerJoined is legal FlatBuffers: the field is absent, so the
	// accessor returns the zero value. It must pass verification (this is not
	// the 4-zero-byte case, which has a bogus root offset).
	b2 := flatbuffers.NewBuilder(32)
	Protocol.PlayerJoinedStart(b2)
	empty := Protocol.PlayerJoinedEnd(b2)
	b2.Finish(empty)
	if _, err := fbVerifiedPlayerJoined(b2.FinishedBytes()); err != nil {
		t.Fatalf("verifier rejected a legal empty PlayerJoined: %v", err)
	}
}

// TestVerifyRootTruncationsAreSafeToDispatch takes a valid buffer and truncates
// it at every length. The verifier is a STRUCTURAL check, not a checksum: a
// truncation can leave a perfectly well-formed (but different) table — e.g.
// dropping the payload field leaves payload_type absent, which reads as NONE.
// So acceptance is not a failure. The invariant that actually matters, and the
// one the whole change exists to guarantee, is:
//
//	every accepted buffer must be safe to hand to the generated accessors.
//
// That is asserted by dispatching each one and requiring no panic — the exact
// failure mode the issue reported ("index out of range [3] with length 0").
func TestVerifyRootTruncationsAreSafeToDispatch(t *testing.T) {
	full := buildGetInventoryReqFrame(t, 1234)
	accepted := 0
	for n := 0; n < len(full); n++ {
		trunc := full[:n]
		frame, err := fbVerifyFrame(trunc)
		if err != nil {
			continue // rejected: good, dropped before any accessor
		}
		accepted++
		// Accepted, so every accessor on it must be in bounds. Reading the
		// fields the handler reads is what used to panic.
		func() {
			defer func() {
				if r := recover(); r != nil {
					t.Fatalf("verifier ACCEPTED a %d-byte truncation of a %d-byte valid frame, but reading it panics: %v", n, len(full), r)
				}
			}()
			_ = frame.PayloadType()
			var tab flatbuffers.Table
			if frame.Payload(&tab) {
				msg := &Protocol.MetaDBMessage{}
				msg.Init(tab.Bytes, tab.Pos)
				_ = msg.ReqId()
				_ = msg.RequestType()
				var reqTab flatbuffers.Table
				if msg.Request(&reqTab) {
					_ = fbRequestDescriptor(msg.RequestType())
				}
			}
		}()
	}
	if accepted == len(full) {
		t.Fatal("verifier accepted every truncation: it is not actually validating")
	}
	// The untruncated buffer must still pass.
	if _, err := fbVerifyFrame(full); err != nil {
		t.Fatalf("verifier rejected the full valid frame: %v", err)
	}
}

// TestVerifyRootRejectsTruncatedPlayerJoined is the same truncation sweep for
// the two previously-unguarded tables, which are the ones the issue calls out
// as parsing 4 zero bytes as player_id 0. Here acceptance of a short buffer IS
// a failure, because a PlayerJoined/PlayerLeft carries only scalars: a
// truncated one can never be a legal table, and accepting it would mean acting
// on a half-read player id or position.
func TestVerifyRootRejectsTruncatedPlayerJoinedAndLeft(t *testing.T) {
	build := func() []byte {
		b := flatbuffers.NewBuilder(64)
		Protocol.PlayerLeftStart(b)
		Protocol.PlayerLeftAddPlayerId(b, 1)
		Protocol.PlayerLeftAddX(b, 2)
		Protocol.PlayerLeftAddY(b, 3)
		Protocol.PlayerLeftAddZ(b, 4)
		off := Protocol.PlayerLeftEnd(b)
		b.Finish(off)
		return b.FinishedBytes()
	}
	full := build()
	for n := 0; n < len(full); n++ {
		trunc := full[:n]
		if _, err := fbVerifiedPlayerLeft(trunc); err == nil {
			t.Fatalf("verifier accepted a %d-byte truncation of a valid PlayerLeft", n)
		}
		if _, err := fbVerifiedPlayerJoined(trunc); err == nil {
			t.Fatalf("verifier accepted a %d-byte truncation as PlayerJoined", n)
		}
	}
}

// buildGetInventoryReqFrame builds a valid MetaDBFrame/MetaDBMessage wrapping a
// GetInventoryReq, used to prove the handler still works after the fix.
func buildGetInventoryReqFrame(t *testing.T, playerID uint64) []byte {
	t.Helper()
	b := flatbuffers.NewBuilder(256)

	Protocol.GetInventoryReqStart(b)
	Protocol.GetInventoryReqAddPlayerId(b, playerID)
	Protocol.GetInventoryReqAddSlotIndex(b, 0)
	reqOff := Protocol.GetInventoryReqEnd(b)

	Protocol.MetaDBMessageStart(b)
	Protocol.MetaDBMessageAddReqId(b, 1)
	Protocol.MetaDBMessageAddRequestType(b, Protocol.MetaDBRequestGetInventoryReq)
	Protocol.MetaDBMessageAddRequest(b, reqOff)
	msgOff := Protocol.MetaDBMessageEnd(b)

	Protocol.MetaDBFrameStart(b)
	Protocol.MetaDBFrameAddPayloadType(b, Protocol.MetaDBPayloadMetaDBMessage)
	Protocol.MetaDBFrameAddPayload(b, msgOff)
	frameOff := Protocol.MetaDBFrameEnd(b)
	b.Finish(frameOff)
	return b.FinishedBytes()
}
