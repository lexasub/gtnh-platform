package testutil

import (
	"testing"

	flatbuffers "github.com/google/flatbuffers/go"
	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
)

// NOTE: In Go FlatBuffers, struct fields must be written AFTER StartObject
// and ALL scalar fields, immediately before AddPos (struct data is inline).
// SetBlockActionOptions contains optional client correlation and placement fields.
//
// Face follows Protocol::Vec3i-adjacent conventions in ActionContext.h:
// 0 = DOWN, 1 = UP, 2 = NORTH, 3 = SOUTH, 4 = WEST, 5 = EAST. Face 0 (DOWN)
// means "clicked the top face of the block below", so the server places at
// (x, y, z) itself — the same cell the client sent.
type SetBlockActionOptions struct {
	RequestID uint32
	Face      byte
	HeldItem  uint16
}

// BuildSetBlockAction builds a RIGHT_MOUSE_CLICK SetBlockAction with an EMPTY
// HAND (held_item = 0).
//
// This is NOT a block placement. SimCore's PlaceBlockHandler::canHandle
// (src/game/actions/handlers/PlaceBlockHandler.cpp:14-18) requires
// held_item != 0, so an empty-hand frame is claimed by no handler and the
// facade answers REJECTED "nothing placeable in hand"
// (src/game/actions/SetBlockCASHandler.cpp:44-51). The production client never
// sends this: NetClient::SendBlockAction
// (src/apps/game_client/Network/NetClient.cpp:689-706) always writes
// held_item = the equipped item.
//
// Use BuildPlaceBlockAction for anything that is meant to place a block.
// This builder remains for raw-protocol and negative-path tests that
// deliberately exercise an empty hand.
func BuildSetBlockAction(playerID uint64, x, y, z int32, expectedBlockID, newBlockID uint16) []byte {
	return BuildSetBlockActionWithOptions(playerID, x, y, z, expectedBlockID, newBlockID, SetBlockActionOptions{})
}

// BuildPlaceBlockAction builds the production placement frame: a
// RIGHT_MOUSE_CLICK whose held_item equals the block being placed, mirroring
// NetClient::SendBlockAction, which writes the equipped item into both
// new_block_id and held_item.
func BuildPlaceBlockAction(playerID uint64, x, y, z int32, expectedBlockID, blockID uint16) []byte {
	return BuildPlaceBlockActionWithOptions(playerID, x, y, z, expectedBlockID, blockID, SetBlockActionOptions{Face: 0, HeldItem: blockID})
}

// BuildPlaceBlockActionWithOptions is BuildPlaceBlockAction with client
// correlation (RequestID) and an explicit face.
func BuildPlaceBlockActionWithOptions(playerID uint64, x, y, z int32, expectedBlockID, blockID uint16, opts SetBlockActionOptions) []byte {
	opts.HeldItem = blockID
	return BuildSetBlockActionWithOptions(playerID, x, y, z, expectedBlockID, blockID, opts)
}

// BuildSetBlockActionWithOptions builds a placement action with request-aware fields.
func BuildSetBlockActionWithOptions(playerID uint64, x, y, z int32, expectedBlockID, newBlockID uint16, opts SetBlockActionOptions) []byte {
	b := flatbuffers.NewBuilder(128)
	Protocol.SetBlockActionStart(b)
	Protocol.SetBlockActionAddPlayerId(b, playerID)
	Protocol.SetBlockActionAddAction(b, Protocol.PlayerActionTypeRIGHT_MOUSE_CLICK)
	pos := Protocol.CreateVec3i(b, x, y, z)
	Protocol.SetBlockActionAddPos(b, pos)
	Protocol.SetBlockActionAddExpectedBlockId(b, expectedBlockID)
	Protocol.SetBlockActionAddNewBlockId(b, newBlockID)
	Protocol.SetBlockActionAddRequestId(b, opts.RequestID)
	Protocol.SetBlockActionAddFace(b, opts.Face)
	Protocol.SetBlockActionAddHeldItem(b, opts.HeldItem)
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
// held_item is left empty: BreakBlockHandler::canHandle only tests
// action_type, and breaking is not a placement, so there is no item in hand
// to report.
func BuildBreakBlockAction(playerID uint64, x, y, z int32, expectedBlockID uint16) []byte {
	return BuildBreakBlockActionWithOptions(playerID, x, y, z, expectedBlockID, SetBlockActionOptions{})
}

// BuildBreakBlockActionWithOptions builds a break action with request correlation.
func BuildBreakBlockActionWithOptions(playerID uint64, x, y, z int32, expectedBlockID uint16, opts SetBlockActionOptions) []byte {
	b := flatbuffers.NewBuilder(128)
	Protocol.SetBlockActionStart(b)
	Protocol.SetBlockActionAddPlayerId(b, playerID)
	Protocol.SetBlockActionAddAction(b, Protocol.PlayerActionTypeLEFT_MOUSE_CLICK)
	pos := Protocol.CreateVec3i(b, x, y, z)
	Protocol.SetBlockActionAddPos(b, pos)
	Protocol.SetBlockActionAddExpectedBlockId(b, expectedBlockID)
	Protocol.SetBlockActionAddNewBlockId(b, 0)
	Protocol.SetBlockActionAddRequestId(b, opts.RequestID)
	Protocol.SetBlockActionAddFace(b, opts.Face)
	Protocol.SetBlockActionAddHeldItem(b, opts.HeldItem)
	action := Protocol.SetBlockActionEnd(b)
	b.Finish(action)
	return b.FinishedBytes()
}

// BuildPlayerAction builds a PlayerAction FlatBuffer (for ITEM_ACTION, CHUNK_REQUEST, etc).
func BuildPlayerAction(playerID uint64, actionType Protocol.PlayerActionType, x, y, z int32, itemID uint16, count byte) []byte {
	b := flatbuffers.NewBuilder(64)
	Protocol.PlayerActionStart(b)
	Protocol.PlayerActionAddPlayerId(b, playerID)
	Protocol.PlayerActionAddAction(b, actionType)
	Protocol.PlayerActionAddBlockId(b, itemID)
	Protocol.PlayerActionAddCount(b, count)
	pos := Protocol.CreateVec3i(b, x, y, z)
	Protocol.PlayerActionAddPos(b, pos)
	act := Protocol.PlayerActionEnd(b)
	b.Finish(act)
	return b.FinishedBytes()
}

// BuildGameModeChange builds the client→server GameModeChange FlatBuffer.
func BuildGameModeChange(playerID uint64, mode Protocol.GameMode) []byte {
	b := flatbuffers.NewBuilder(32)
	Protocol.GameModeChangeStart(b)
	Protocol.GameModeChangeAddPlayerId(b, playerID)
	Protocol.GameModeChangeAddNewMode(b, mode)
	change := Protocol.GameModeChangeEnd(b)
	b.Finish(change)
	return b.FinishedBytes()
}

// BuildQuestBookOpen builds the client→server QuestBookOpen FlatBuffer.
func BuildQuestBookOpen(playerID uint64) []byte {
	b := flatbuffers.NewBuilder(32)
	Protocol.QuestBookOpenStart(b)
	Protocol.QuestBookOpenAddPlayerId(b, playerID)
	req := Protocol.QuestBookOpenEnd(b)
	b.Finish(req)
	return b.FinishedBytes()
}

// BuildStartScenarioReq builds the client→server StartScenarioReq FlatBuffer.
func BuildStartScenarioReq(playerID uint64, scenarioIndex byte) []byte {
	b := flatbuffers.NewBuilder(32)
	Protocol.StartScenarioReqStart(b)
	Protocol.StartScenarioReqAddPlayerId(b, playerID)
	Protocol.StartScenarioReqAddScenarioIndex(b, scenarioIndex)
	req := Protocol.StartScenarioReqEnd(b)
	b.Finish(req)
	return b.FinishedBytes()
}

// BuildInventoryActionWithOptions builds a complete InventoryAction using the
// current authoritative container-click schema.
func BuildInventoryActionWithOptions(playerID uint64, actionType, button, mods, containerID byte, slot uint16, count byte) []byte {
	b := flatbuffers.NewBuilder(64)
	Protocol.InventoryActionStart(b)
	Protocol.InventoryActionAddPlayerId(b, playerID)
	Protocol.InventoryActionAddActionType(b, actionType)
	Protocol.InventoryActionAddButton(b, button)
	Protocol.InventoryActionAddMods(b, mods)
	Protocol.InventoryActionAddContainerId(b, containerID)
	Protocol.InventoryActionAddSlot(b, slot)
	Protocol.InventoryActionAddCount(b, count)
	action := Protocol.InventoryActionEnd(b)
	b.Finish(action)
	return b.FinishedBytes()
}

// BuildInventoryAction builds an InventoryAction FlatBuffer.
// The current schema represents the source slot as Slot; targetSlot and meta
// remain parameters for compatibility with older callers.
func BuildInventoryAction(playerID uint64, actionType uint8, sourceSlot, targetSlot int16, count uint8, meta uint16) []byte {
	b := flatbuffers.NewBuilder(64)
	Protocol.InventoryActionStart(b)
	Protocol.InventoryActionAddPlayerId(b, playerID)
	Protocol.InventoryActionAddActionType(b, actionType)
	Protocol.InventoryActionAddSlot(b, uint16(sourceSlot))
	Protocol.InventoryActionAddCount(b, count)
	act := Protocol.InventoryActionEnd(b)
	b.Finish(act)
	return b.FinishedBytes()
}

// BuildSetMachineSlotReq builds a SetMachineSlotReq FlatBuffer.
func BuildContainerOpenReq(playerID uint64, x, y, z int32) []byte {
	b := flatbuffers.NewBuilder(64)
	Protocol.ContainerOpenReqStart(b)
	Protocol.ContainerOpenReqAddPlayerId(b, playerID)
	pos := Protocol.CreateVec3i(b, x, y, z)
	Protocol.ContainerOpenReqAddPos(b, pos)
	req := Protocol.ContainerOpenReqEnd(b)
	b.Finish(req)
	return b.FinishedBytes()
}

// BuildToolAction builds a ToolAction FlatBuffer for wrench/drill etc.
func BuildToolAction(playerID uint64, actionType uint8, x, y, z int32, face uint8, itemID uint16) []byte {
	b := flatbuffers.NewBuilder(64)
	Protocol.ToolActionStart(b)
	Protocol.ToolActionAddPlayerId(b, playerID)
	Protocol.ToolActionAddAction(b, Protocol.ToolActionType(actionType))
	pos := Protocol.CreateVec3i(b, x, y, z)
	Protocol.ToolActionAddPos(b, pos)
	Protocol.ToolActionAddFace(b, face)
	Protocol.ToolActionAddItemId(b, itemID)
	Protocol.ToolActionAddSlotIdx(b, 0)
	Protocol.ToolActionAddExtraData(b, 0)
	act := Protocol.ToolActionEnd(b)
	b.Finish(act)
	return b.FinishedBytes()
}

// BuildPipeContentsReq builds a PipeContentsReq FlatBuffer.
func BuildPipeContentsReq(playerID uint64, x, y, z int32) []byte {
	b := flatbuffers.NewBuilder(64)
	Protocol.PipeContentsReqStart(b)
	Protocol.PipeContentsReqAddPlayerId(b, playerID)
	pos := Protocol.CreateVec3i(b, x, y, z)
	Protocol.PipeContentsReqAddPos(b, pos)
	req := Protocol.PipeContentsReqEnd(b)
	b.Finish(req)
	return b.FinishedBytes()
}

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
