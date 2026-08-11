package testutil

import (
	"testing"

	flatbuffers "github.com/google/flatbuffers/go"
	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
)

// BuildSetBlockAction builds a SetBlockAction FlatBuffer.
// NOTE: In Go FlatBuffers, struct fields must be written AFTER StartObject
// and ALL scalar fields, immediately before AddPos (struct data is inline).
// BuildSetBlockAction builds a SetBlockAction FlatBuffer for placing a block.
// Uses RIGHT_MOUSE_CLICK (place). For breaking blocks, use BuildBreakBlockAction.
func BuildSetBlockAction(playerID uint64, x, y, z int32, expectedBlockID, newBlockID uint16) []byte {
	b := flatbuffers.NewBuilder(128)
	Protocol.SetBlockActionStart(b)
	// Scalar fields first (any order among themselves)
	Protocol.SetBlockActionAddPlayerId(b, playerID)
	Protocol.SetBlockActionAddAction(b, Protocol.PlayerActionTypeRIGHT_MOUSE_CLICK)
	// Struct field: write data then immediately record vtable position
	pos := Protocol.CreateVec3i(b, x, y, z)
	Protocol.SetBlockActionAddPos(b, pos)
	// Remaining scalar fields
	Protocol.SetBlockActionAddExpectedBlockId(b, expectedBlockID)
	Protocol.SetBlockActionAddNewBlockId(b, newBlockID)
	action := Protocol.SetBlockActionEnd(b)
	b.Finish(action)
	return b.FinishedBytes()
}

// BuildCraftRequest builds a CraftRequest for a 3x3 grid (9 ItemStacks).
// grid is [itemID, count, meta] triples, exactly 9*3 = 27 ints.
func BuildCraftRequest(playerID uint64, x, y, z int32, grid [][3]uint16) []byte {
	b := flatbuffers.NewBuilder(256)

	// Build grid vector first (UOffsetT — can be built before the table)
	n := 9
	if len(grid) < n {
		n = len(grid)
	}
	Protocol.CraftRequestStartSlotsVector(b, n)
	for i := n - 1; i >= 0; i-- {
		itemID := uint16(0)
		count := byte(0)
		meta := uint16(0)
		if i < len(grid) {
			itemID = grid[i][0]
			count = byte(grid[i][1])
			meta = grid[i][2]
		}
		b.PrependUint16(meta)
		b.Pad(1)
		b.PrependByte(count)
		b.PrependUint16(itemID)
	}
	slots := b.EndVector(6)

	Protocol.CraftRequestStart(b)
	Protocol.CraftRequestAddPlayerId(b, playerID)
	// Inline struct (Vec3i) MUST be created inside the table's Start/End block
	pos := Protocol.CreateVec3i(b, x, y, z)
	Protocol.CraftRequestAddPos(b, pos)
	Protocol.CraftRequestAddSlots(b, slots)
	req := Protocol.CraftRequestEnd(b)
	b.Finish(req)
	return b.FinishedBytes()
}

// AssertBlockAck checks that a BlockAck has the expected status.
func AssertBlockAck(t *testing.T, data []byte, expectedStatus Protocol.BlockAckStatus) {
	t.Helper()
	ack := Protocol.GetRootAsBlockAck(data, 0)
	if ack == nil {
		t.Fatal("BlockAck: nil root")
	}
	if ack.Status() != expectedStatus {
		t.Errorf("BlockAck status: expected %v, got %v", expectedStatus, ack.Status())
	}
}

// AssertCraftResponse checks that a CraftResponse has the expected success value.
func AssertCraftResponse(t *testing.T, data []byte, expectSuccess bool) *Protocol.CraftResponse {
	t.Helper()
	resp := Protocol.GetRootAsCraftResponse(data, 0)
	if resp == nil {
		t.Fatal("CraftResponse: nil root")
	}
	if resp.Success() != expectSuccess {
		t.Errorf("CraftResponse success: expected %v, got %v (error: %s)",
			expectSuccess, resp.Success(), string(resp.Error()))
	}
	return resp
}

// BuildBreakBlockAction builds a SetBlockAction FlatBuffer for breaking a block.
// Uses LEFT_MOUSE_CLICK (break). expectedBlockID = 0 means "any block".
func BuildBreakBlockAction(playerID uint64, x, y, z int32, expectedBlockID uint16) []byte {
	b := flatbuffers.NewBuilder(128)
	Protocol.SetBlockActionStart(b)
	Protocol.SetBlockActionAddPlayerId(b, playerID)
	Protocol.SetBlockActionAddAction(b, Protocol.PlayerActionTypeLEFT_MOUSE_CLICK)
	pos := Protocol.CreateVec3i(b, x, y, z)
	Protocol.SetBlockActionAddPos(b, pos)
	Protocol.SetBlockActionAddExpectedBlockId(b, expectedBlockID)
	Protocol.SetBlockActionAddNewBlockId(b, 0) // break → 0 = air
	action := Protocol.SetBlockActionEnd(b)
	b.Finish(action)
	return b.FinishedBytes()
}

// BuildPlayerAction builds a PlayerAction FlatBuffer (for ITEM_ACTION, CHUNK_REQUEST, etc).
func BuildPlayerAction(playerID uint64, actionType Protocol.PlayerActionType, x, y, z int32, itemID uint16, count byte) []byte {
	b := flatbuffers.NewBuilder(64)
	pos := Protocol.CreateVec3i(b, x, y, z)
	Protocol.PlayerActionStart(b)
	Protocol.PlayerActionAddPlayerId(b, playerID)
	Protocol.PlayerActionAddAction(b, actionType)
	Protocol.PlayerActionAddPos(b, pos)
	Protocol.PlayerActionAddBlockId(b, itemID)
	Protocol.PlayerActionAddCount(b, count)
	act := Protocol.PlayerActionEnd(b)
	b.Finish(act)
	return b.FinishedBytes()
}

// BuildInventoryAction builds an InventoryAction FlatBuffer.
func BuildInventoryAction(playerID uint64, actionType uint8, sourceSlot, targetSlot int16, count uint8, meta uint16) []byte {
	b := flatbuffers.NewBuilder(64)
	Protocol.InventoryActionStart(b)
	Protocol.InventoryActionAddPlayerId(b, playerID)
	Protocol.InventoryActionAddActionType(b, actionType)
	Protocol.InventoryActionAddSourceSlot(b, byte(sourceSlot))
	Protocol.InventoryActionAddTargetSlot(b, byte(targetSlot))
	Protocol.InventoryActionAddCount(b, count)
	Protocol.InventoryActionAddMeta(b, meta)
	act := Protocol.InventoryActionEnd(b)
	b.Finish(act)
	return b.FinishedBytes()
}

// BuildSetMachineSlotReq builds a SetMachineSlotReq FlatBuffer.
func BuildSetMachineSlotReq(playerID uint64, x, y, z int32, slotIndex uint16, itemID uint16, count byte, meta uint16, playerSlot byte) []byte {
	b := flatbuffers.NewBuilder(64)
	Protocol.SetMachineSlotReqStart(b)
	Protocol.SetMachineSlotReqAddPlayerId(b, playerID)
	pos := Protocol.CreateVec3i(b, x, y, z)
	Protocol.SetMachineSlotReqAddPos(b, pos)
	Protocol.SetMachineSlotReqAddSlotIndex(b, slotIndex)
	Protocol.SetMachineSlotReqAddItemId(b, itemID)
	Protocol.SetMachineSlotReqAddCount(b, count)
	Protocol.SetMachineSlotReqAddMeta(b, meta)
	Protocol.SetMachineSlotReqAddPlayerSlot(b, playerSlot)
	req := Protocol.SetMachineSlotReqEnd(b)
	b.Finish(req)
	return b.FinishedBytes()
}
