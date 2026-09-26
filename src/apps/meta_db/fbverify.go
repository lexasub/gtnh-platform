// gp-3v5x: structural FlatBuffer validation for the MetaDB boundary.
//
// The C++ side already runs flatbuffers::Verifier before GetRoot — see
// src/game/storage/InventoryLoadHandler.cpp:21-26 and
// src/game/storage/PlayerJoinedHandler.cpp:22-27 (both fixed by gp-ajvg after
// a real daemon segfault). The Go side has the same hazard with no equivalent
// guard: github.com/google/flatbuffers/go ships NO Verifier at all (the go/
// package contains builder, encode, grpc, lib, sizes, struct, table — there is
// no verifier.go, and `grep -rn "type Verifier" go/` returns nothing).
//
// GetRootAsX therefore never returns nil; it either returns a *Table whose
// accessors later slice out of the buffer, or it panics inside GetInt32 when
// the root offset points past the end. The audit's live probe produced
// "index out of range [3] with length 0" on exactly this path.
//
// verifyRoot is the Go counterpart of flatbuffers::Verifier::VerifyBuffer<T>:
// it walks the root offset and vtable of every table a handler will actually
// read and bounds-checks every field, so a buffer that passes it can never make
// a generated accessor slice out of range. It returns an error rather than
// panicking, which lets the caller drop the frame quietly instead of unwinding.
//
// Design constraint (house rule): validation is expressed purely in terms of
// the EXISTING .fbs tables. No schema change, no invented fields.
package main

import (
	"encoding/binary"
	"fmt"

	flatbuffers "github.com/google/flatbuffers/go"

	"github.com/gtnh-platform/protocol/generated/go/Protocol"
)

// fbFieldKind enumerates the FlatBuffers field encodings that appear in the
// MetaDB tables this file validates.
type fbFieldKind int

const (
	fbU8 fbFieldKind = iota // uint8, byte, bool, and all enums (fb enums are byte-sized)
	fbU16
	fbU32
	fbU64
	fbS32
	fbTableRef    // inline table/union offset: [u32 forward offset]
	fbTableVector // vector of tables: [u32 len][u32 forward offset]*
	fbString      // string: [u32 len][utf8]*
	fbBytes       // byte vector: [u32 len][byte]*
)

// fbField is one verified vtable slot: the index the generated accessor passes
// to Table.Offset, the field's encoding, and — for table/vector fields — the
// concrete type the offset points at.
type fbField struct {
	slot flatbuffers.VOffsetT
	kind fbFieldKind
	// nested is the concrete table type this field's offset/vector points at.
	// For a union this depends on the discriminator in a sibling field, so it is
	// resolved by nestedFor, which receives the table's own buffer and
	// position and returns the descriptor to verify the target against (nil to
	// accept any well-formed table header).
	//
	// A union with a nil nestedFor is NOT safe to leave header-only: the
	// handler Init()s the concrete type on the target and reads its fields
	// immediately. Header-only would let a well-formed table whose fields
	// point past the end through, which is the exact 40-byte-truncation case
	// below. Every union field here must carry a nestedFor.
	nested    *fbTable
	nestedFor func(buf []byte, tablePos flatbuffers.UOffsetT) *fbTable
}

// fbTable describes one .fbs table.
type fbTable struct {
	name   string
	fields []fbField
}

// Descriptor tables. Every slot and width below was read off the generated
// accessors, not guessed: e.g. PlayerJoined.PlayerId is Offset(4)/GetUint64,
// PlayerLeft.X is Offset(6)/GetInt32, and MetaDBFrame.Payload (a union) is
// Offset(6) consumed through Table.Union. QuestProgressUpdate.Quests is
// Offset(6) consumed through Table.VectorLen/Table.Vector.
var (
	fbMetaDBFrame = &fbTable{
		name: "MetaDBFrame",
		fields: []fbField{
			{slot: 4, kind: fbU8}, // payload_type : MetaDBPayload (byte enum)
			// payload is a union, so the concrete type comes from payload_type.
			// dispatchFlatBufferFrame Init()s a MetaDBMessage on it and reads
			// req_id immediately, so it must be verified as one.
			{slot: 6, kind: fbTableRef, nestedFor: fbMetaDBFramePayload},
		},
	}

	fbMetaDBMessage = &fbTable{
		name: "MetaDBMessage",
		fields: []fbField{
			{slot: 4, kind: fbU32}, // req_id : uint32
			{slot: 6, kind: fbU8},  // request_type : MetaDBRequest (byte enum)
			// request is a union keyed on request_type; handleMetaDBMessage
			// Init()s the concrete request and reads its scalars immediately.
			{slot: 8, kind: fbTableRef, nestedFor: fbMetaDBMessageRequest},
		},
	}

	fbGetInventoryReq = &fbTable{
		name: "GetInventoryReq",
		fields: []fbField{
			{slot: 4, kind: fbU64}, // player_id : uint64
			{slot: 6, kind: fbU16}, // slot_index : uint16
		},
	}

	fbSetInventorySlotReq = &fbTable{
		name: "SetInventorySlotReq",
		fields: []fbField{
			{slot: 4, kind: fbU64},  // player_id : uint64
			{slot: 6, kind: fbU16},  // slot_index : uint16
			{slot: 8, kind: fbU16},  // item_id : uint16
			{slot: 10, kind: fbU8},  // count : uint8
			{slot: 12, kind: fbU16}, // meta : uint16
		},
	}

	fbGetInventorySnapshotReq = &fbTable{
		name: "GetInventorySnapshotReq",
		fields: []fbField{
			{slot: 4, kind: fbU64}, // player_id : uint64
			{slot: 6, kind: fbU8},  // include_hotbar : bool
		},
	}

	fbQuestProgressUpdate = &fbTable{
		name: "QuestProgressUpdate",
		fields: []fbField{
			{slot: 4, kind: fbU64},                               // player_id : uint64
			{slot: 6, kind: fbTableVector, nested: fbQuestEntry}, // quests : [QuestEntry]
		},
	}

	fbQuestEntry = &fbTable{
		name: "QuestEntry",
		fields: []fbField{
			{slot: 4, kind: fbU32}, // quest_id : uint32
			{slot: 6, kind: fbU8},  // status : QuestStatus (byte enum)
			{slot: 8, kind: fbU8},  // progress : uint8
		},
	}

	fbQuestExchangeRequest = &fbTable{
		name: "QuestExchangeRequest",
		fields: []fbField{
			{slot: 4, kind: fbU64}, // player_id : uint64
			{slot: 6, kind: fbU32}, // quest_id : uint32
		},
	}

	fbQuestExchangeCooldownGet = &fbTable{
		name: "QuestExchangeCooldownGet",
		fields: []fbField{
			{slot: 4, kind: fbU64}, // player_id : uint64
			{slot: 6, kind: fbU32}, // quest_id : uint32
		},
	}

	fbQuestCompleted = &fbTable{
		name: "QuestCompleted",
		fields: []fbField{
			{slot: 4, kind: fbU64}, // player_id : uint64
			{slot: 6, kind: fbU32}, // quest_id : uint32
			{slot: 8, kind: fbU64}, // timestamp : uint64
		},
	}

	fbPlayerJoined = &fbTable{
		name: "PlayerJoined",
		fields: []fbField{
			{slot: 4, kind: fbU64}, // player_id : uint64
		},
	}

	fbPlayerLeft = &fbTable{
		name: "PlayerLeft",
		fields: []fbField{
			{slot: 4, kind: fbU64},  // player_id : uint64
			{slot: 6, kind: fbS32},  // x : int32
			{slot: 8, kind: fbS32},  // y : int32
			{slot: 10, kind: fbS32}, // z : int32
		},
	}
)

// fbFieldWidth is the on-buffer width of each scalar encoding, in bytes. These
// are the widths the generated Table.Get* accessors read.
var fbFieldWidth = map[fbFieldKind]int{
	fbU8:  1,
	fbU16: 2,
	fbU32: 4,
	fbU64: 8,
	fbS32: 4,
}

// fbMinTableSize is the smallest legal table: a 4-byte soffset to its vtable.
const fbMinTableSize = 4

// fbMaxDepth bounds nested-table recursion so a self-referential or otherwise
// pathological buffer cannot drive unbounded work.
const fbMaxDepth = 8

// verifyRoot is the Go counterpart of flatbuffers::Verifier::VerifyBuffer<T>.
// It returns the absolute position of the root table, or an error explaining
// why the buffer is unsafe to hand to a generated accessor.
//
// R1 (empty buffer): len(buf) < 4 means there is no root offset at all. This is
// the exact case the C++ fix rejects first — for an empty std::vector,
// data.data() is nullptr, so GetRoot handed the handler a Table whose first
// accessor dereferenced null.
//
// R2 (bogus-but-nonempty): a garbage buffer is just as bad, because it makes
// the handler act on fabricated data (a wrong-player read or write). Every
// field is bounds-checked so that is rejected before any accessor runs.
func verifyRoot(buf []byte, desc *fbTable) (flatbuffers.UOffsetT, error) {
	if len(buf) < 4 {
		return 0, fmt.Errorf("%s: buffer too short for root offset: %d bytes", desc.name, len(buf))
	}
	rootOff := flatbuffers.UOffsetT(binary.LittleEndian.Uint32(buf[0:4]))
	if err := verifyTableAt(buf, rootOff, desc, 0); err != nil {
		return 0, err
	}
	return rootOff, nil
}

// fbReadScalarSlot reads the byte-sized vtable slot off a table whose vtable
// has already been bounds-checked, returning the field's value or 0 when the
// field is absent (which is what the generated accessor does too). It is only
// safe to call from a nestedFor callback: verifyTableAt has already validated
// the vtable and this field's width, so the read cannot go out of range.
func fbReadScalarSlot(buf []byte, tablePos flatbuffers.UOffsetT, slot flatbuffers.VOffsetT) byte {
	soffset := int32(binary.LittleEndian.Uint32(buf[tablePos : tablePos+4]))
	vtablePos := int64(tablePos) - int64(soffset)
	vtableLen := int(binary.LittleEndian.Uint16(buf[vtablePos : vtablePos+2]))
	if int(slot)+2 > vtableLen {
		return 0
	}
	rel := flatbuffers.VOffsetT(binary.LittleEndian.Uint16(buf[vtablePos+int64(slot) : vtablePos+int64(slot)+2]))
	if rel == 0 {
		return 0
	}
	return buf[int64(tablePos)+int64(rel)]
}

// fbMetaDBFramePayload resolves the MetaDBFrame.payload union target from the
// frame's own payload_type discriminator (vtable slot 4).
//
// Note the ordering dependency: this callback runs while the union field is
// being verified, and it reads slot 4, so payload_type MUST be declared before
// payload in fbMetaDBFrame.fields — otherwise the read is unvalidated. Same
// rule for request_type before request in fbMetaDBMessage.
//
// Returning nil means "verify the target as a bare table header": correct only
// for a union member no handler reads (an inbound MetaDBReply is dropped by
// dispatchFlatBufferFrame before any field access).
func fbMetaDBFramePayload(buf []byte, tablePos flatbuffers.UOffsetT) *fbTable {
	switch fbReadScalarSlot(buf, tablePos, 4) {
	case byte(Protocol.MetaDBPayloadMetaDBMessage):
		return fbMetaDBMessage
	default:
		// NONE / MetaDBReply / any unknown tag: dispatchFlatBufferFrame either
		// logs "unknown payload type" or "unexpected reply frame" and returns
		// without touching the union target, so header-only is sound here.
		return nil
	}
}

// fbMetaDBMessageRequest resolves the MetaDBMessage.request union target from
// the message's own request_type discriminator (vtable slot 6).
func fbMetaDBMessageRequest(buf []byte, tablePos flatbuffers.UOffsetT) *fbTable {
	return fbRequestDescriptor(Protocol.MetaDBRequest(fbReadScalarSlot(buf, tablePos, 6)))
}

// verifyTableAt bounds-checks the table at absolute position pos: its vtable,
// every present field, and recursively the nested tables it references.
func verifyTableAt(buf []byte, pos flatbuffers.UOffsetT, desc *fbTable, depth int) error {
	if depth > fbMaxDepth {
		return fmt.Errorf("%s: nesting deeper than %d", desc.name, fbMaxDepth)
	}
	if int64(pos) < 0 || int64(pos)+fbMinTableSize > int64(len(buf)) {
		return fmt.Errorf("%s: table at %d out of bounds (len=%d)", desc.name, pos, len(buf))
	}

	// The vtable sits BEFORE the table: vtablePos = pos - soffset. This is the
	// read that panicked in the audit probe — Table.Offset calls GetSOffsetT
	// (a 4-byte GetInt32) at a position derived from unvalidated bytes.
	soffset := int32(binary.LittleEndian.Uint32(buf[pos : pos+4]))
	vtablePos := int64(pos) - int64(soffset)
	if vtablePos < 0 || vtablePos+4 > int64(len(buf)) {
		return fmt.Errorf("%s: vtable at %d out of bounds (len=%d)", desc.name, vtablePos, len(buf))
	}

	// vtable[0] is the vtable's own length in bytes; Table.Offset compares the
	// requested slot against it, so it must at least cover the 4-byte header.
	vtableLen := int(binary.LittleEndian.Uint16(buf[vtablePos : vtablePos+2]))
	if vtableLen < 4 || vtablePos+int64(vtableLen) > int64(len(buf)) {
		return fmt.Errorf("%s: bad vtable length %d at %d (len=%d)", desc.name, vtableLen, vtablePos, len(buf))
	}

	for _, f := range desc.fields {
		fieldPos, present, err := fbFieldPos(buf, pos, flatbuffers.UOffsetT(vtablePos), vtableLen, f)
		if err != nil {
			return fmt.Errorf("%s: %w", desc.name, err)
		}
		if !present {
			// Absent field: the generated accessor returns the zero value, so
			// there is nothing to bounds-check. Legal FlatBuffers.
			continue
		}
		if err := verifyField(buf, f, fieldPos, pos, depth); err != nil {
			return fmt.Errorf("%s: %w", desc.name, err)
		}
	}
	return nil
}

// fbFieldPos resolves a field's absolute position through the vtable. It
// mirrors Table.Offset exactly: the vtable entry is consulted only when the
// slot falls inside the vtable's own length, and a zero entry means "use the
// field's default".
func fbFieldPos(buf []byte, pos, vtablePos flatbuffers.UOffsetT, vtableLen int, f fbField) (flatbuffers.UOffsetT, bool, error) {
	if int(f.slot)+2 > vtableLen {
		return 0, false, nil
	}
	entry := int64(vtablePos) + int64(f.slot)
	rel := flatbuffers.VOffsetT(binary.LittleEndian.Uint16(buf[entry : entry+2]))
	if rel == 0 {
		return 0, false, nil
	}
	fieldPos := int64(pos) + int64(rel)
	if fieldPos < 0 || fieldPos > int64(len(buf)) {
		return 0, false, fmt.Errorf("field at slot %d resolves to %d, out of bounds (len=%d)", f.slot, fieldPos, len(buf))
	}
	return flatbuffers.UOffsetT(fieldPos), true, nil
}

// verifyField bounds-checks one present field and recurses into nested tables
// and vectors. Scalar widths match the generated Get* accessors, so a field
// that passes here cannot make its accessor slice out of range. tablePos is the
// owning table's absolute position, which a union field needs to resolve its
// discriminator before descending.
func verifyField(buf []byte, f fbField, pos, tablePos flatbuffers.UOffsetT, depth int) error {
	switch f.kind {
	case fbU8, fbU16, fbU32, fbU64, fbS32:
		w := fbFieldWidth[f.kind]
		if int64(pos)+int64(w) > int64(len(buf)) {
			return fmt.Errorf("scalar at %d needs %d bytes, buffer len %d", pos, w, len(buf))
		}
		return nil

	case fbTableRef:
		// Table.Union / Table.Indirect: a uint32 forward offset to a nested
		// table. Follow it exactly as the generated accessor does, then verify
		// the target against its concrete descriptor.
		target, err := fbFollowRef(buf, pos)
		if err != nil {
			return err
		}
		nested := f.nested
		if f.nestedFor != nil {
			nested = f.nestedFor(buf, tablePos)
		}
		if nested != nil {
			return verifyTableAt(buf, flatbuffers.UOffsetT(target), nested, depth+1)
		}
		return nil

	case fbTableVector:
		// Vector of tables: [u32 len][u32 forward offset]*. The length is
		// checked against the remaining bytes BEFORE being used to bound the
		// element loop, so a bogus count cannot make the check itself overrun.
		vecStart, err := fbFollowRef(buf, pos)
		if err != nil {
			return err
		}
		if vecStart+4 > int64(len(buf)) {
			return fmt.Errorf("vector length prefix at %d out of bounds (len=%d)", vecStart, len(buf))
		}
		n := int64(binary.LittleEndian.Uint32(buf[vecStart : vecStart+4]))
		if n < 0 || n > (int64(len(buf))-vecStart-4)/4 {
			return fmt.Errorf("vector length %d does not fit in buffer (len=%d)", n, len(buf))
		}
		if f.nested != nil {
			for i := int64(0); i < n; i++ {
				offPos := vecStart + 4 + i*4
				elemPos := offPos + int64(flatbuffers.UOffsetT(binary.LittleEndian.Uint32(buf[offPos:offPos+4])))
				if elemPos < 0 || elemPos+fbMinTableSize > int64(len(buf)) {
					return fmt.Errorf("vector element %d table at %d out of bounds (len=%d)", i, elemPos, len(buf))
				}
				if err := verifyTableAt(buf, flatbuffers.UOffsetT(elemPos), f.nested, depth+1); err != nil {
					return err
				}
			}
		}
		return nil

	case fbString, fbBytes:
		// [u32 len][data]*. Only the length prefix and the data span matter to
		// the accessor; bound both so the slice cannot run past the buffer.
		vecStart, err := fbFollowRef(buf, pos)
		if err != nil {
			return err
		}
		if vecStart+4 > int64(len(buf)) {
			return fmt.Errorf("vector length prefix at %d out of bounds (len=%d)", vecStart, len(buf))
		}
		n := int64(binary.LittleEndian.Uint32(buf[vecStart : vecStart+4]))
		if n < 0 || vecStart+4+n > int64(len(buf)) {
			return fmt.Errorf("vector length %d does not fit in buffer (len=%d)", n, len(buf))
		}
		return nil
	}
	return fmt.Errorf("unknown field kind %d", f.kind)
}

// fbFollowRef reads the uint32 forward offset at pos and returns the absolute
// position it points at, as Table.Indirect/Table.Union compute it.
func fbFollowRef(buf []byte, pos flatbuffers.UOffsetT) (int64, error) {
	if int64(pos)+4 > int64(len(buf)) {
		return 0, fmt.Errorf("offset at %d needs 4 bytes, buffer len %d", pos, len(buf))
	}
	rel := int64(flatbuffers.UOffsetT(binary.LittleEndian.Uint32(buf[pos : pos+4])))
	target := int64(pos) + rel
	if target < 0 || target+fbMinTableSize > int64(len(buf)) {
		return 0, fmt.Errorf("reference at %d points to %d, out of bounds (len=%d)", pos, target, len(buf))
	}
	return target, nil
}

// ---------------------------------------------------------------------------
// Frame-level entry points
//
// MetaDBFrame and MetaDBMessage each carry a union whose concrete type is only
// known after the discriminator is read, so the frame is verified in two steps:
// verify the outer tables, dispatch on payload_type / request_type, then verify
// the concrete request table before any of its accessors run.
// ---------------------------------------------------------------------------

// fbVerifyFrame validates a MetaDBFrame buffer and returns a MetaDBFrame whose
// accessors are in bounds. Callers must still check PayloadType() and resolve
// the union before touching the payload table.
func fbVerifyFrame(buf []byte) (*Protocol.MetaDBFrame, error) {
	off, err := verifyRoot(buf, fbMetaDBFrame)
	if err != nil {
		return nil, err
	}
	frame := &Protocol.MetaDBFrame{}
	frame.Init(buf, off)
	return frame, nil
}

// fbRequestDescriptor maps a MetaDBRequest discriminator to its table
// descriptor. Returns nil for unknown request types, which the handler then
// rejects as an unknown request.
func fbRequestDescriptor(reqType Protocol.MetaDBRequest) *fbTable {
	switch reqType {
	case Protocol.MetaDBRequestGetInventoryReq:
		return fbGetInventoryReq
	case Protocol.MetaDBRequestSetInventorySlotReq:
		return fbSetInventorySlotReq
	case Protocol.MetaDBRequestGetInventorySnapshotReq:
		return fbGetInventorySnapshotReq
	default:
		return nil
	}
}

// fbVerifyRequest validates a MetaDBMessage buffer plus the concrete request
// table it points at, and returns the request's absolute position. Both the
// message and the request are checked before the handler reads either, which is
// the same R1/R2 discipline as the C++ Verifier-before-GetRoot fixes.
func fbVerifyRequest(buf []byte) (msgPos, reqPos flatbuffers.UOffsetT, reqType Protocol.MetaDBRequest, err error) {
	off, err := verifyRoot(buf, fbMetaDBMessage)
	if err != nil {
		return 0, 0, 0, err
	}
	msg := &Protocol.MetaDBMessage{}
	msg.Init(buf, off)

	reqType = msg.RequestType()
	desc := fbRequestDescriptor(reqType)
	if desc == nil {
		// Not an error: the handler reports "unknown request type" itself. The
		// message envelope is still valid, so returning here is safe.
		return off, 0, reqType, nil
	}

	var reqTab flatbuffers.Table
	if !msg.Request(&reqTab) {
		return off, 0, reqType, fmt.Errorf("MetaDBMessage: request_type=%v present but request offset missing", reqType)
	}
	if err := verifyTableAt(buf, reqTab.Pos, desc, 1); err != nil {
		return off, 0, reqType, err
	}
	return off, reqTab.Pos, reqType, nil
}

// ---------------------------------------------------------------------------
// Verified-root helpers for the sidecar topics
//
// Each of these replaces a `GetRootAsX(payload, 0)` + `if x == nil` pair. The
// nil check could never fire, so it implied validation that did not exist;
// these verify the buffer against the table's .fbs descriptor and return an
// error the handler already knows how to log and bail on.
// ---------------------------------------------------------------------------

// fbVerifiedQuestProgressUpdate validates and returns a QuestProgressUpdate.
func fbVerifiedQuestProgressUpdate(buf []byte) (*Protocol.QuestProgressUpdate, error) {
	off, err := verifyRoot(buf, fbQuestProgressUpdate)
	if err != nil {
		return nil, err
	}
	obj := &Protocol.QuestProgressUpdate{}
	obj.Init(buf, off)
	return obj, nil
}

// fbVerifiedQuestCompleted validates and returns a QuestCompleted.
func fbVerifiedQuestCompleted(buf []byte) (*Protocol.QuestCompleted, error) {
	off, err := verifyRoot(buf, fbQuestCompleted)
	if err != nil {
		return nil, err
	}
	obj := &Protocol.QuestCompleted{}
	obj.Init(buf, off)
	return obj, nil
}

// fbVerifiedQuestExchangeRequest validates and returns a
// QuestExchangeRequest.
func fbVerifiedQuestExchangeRequest(buf []byte) (*Protocol.QuestExchangeRequest, error) {
	off, err := verifyRoot(buf, fbQuestExchangeRequest)
	if err != nil {
		return nil, err
	}
	obj := &Protocol.QuestExchangeRequest{}
	obj.Init(buf, off)
	return obj, nil
}

// fbVerifiedQuestExchangeCooldownGet validates and returns a
// QuestExchangeCooldownGet.
func fbVerifiedQuestExchangeCooldownGet(buf []byte) (*Protocol.QuestExchangeCooldownGet, error) {
	off, err := verifyRoot(buf, fbQuestExchangeCooldownGet)
	if err != nil {
		return nil, err
	}
	obj := &Protocol.QuestExchangeCooldownGet{}
	obj.Init(buf, off)
	return obj, nil
}

// fbVerifiedPlayerJoined validates and returns a PlayerJoined.
//
// gp-3v5x: handlePlayerJoined had NO guard at all. "player.joined" is a
// router-subscribed topic, so ANY publisher on the bus can deliver a zero-length
// or truncated payload. Verified here for the same reason the C++ side runs a
// Verifier in PlayerJoinedHandler.cpp:22-27: without it, 4 zero bytes parse
// silently as PlayerJoined{player_id: 0}, i.e. a wrong-player read.
func fbVerifiedPlayerJoined(buf []byte) (*Protocol.PlayerJoined, error) {
	off, err := verifyRoot(buf, fbPlayerJoined)
	if err != nil {
		return nil, err
	}
	obj := &Protocol.PlayerJoined{}
	obj.Init(buf, off)
	return obj, nil
}

// fbVerifiedPlayerLeft validates and returns a PlayerLeft.
//
// gp-3v5x: handlePlayerLeft had NO guard at all, the counterpart of the C++
// fix. A 4-zero-byte payload parsed as PlayerLeft{player_id: 0, 0, 0, 0} and
// wrote a fabricated position over the real player's saved position.
func fbVerifiedPlayerLeft(buf []byte) (*Protocol.PlayerLeft, error) {
	off, err := verifyRoot(buf, fbPlayerLeft)
	if err != nil {
		return nil, err
	}
	obj := &Protocol.PlayerLeft{}
	obj.Init(buf, off)
	return obj, nil
}
