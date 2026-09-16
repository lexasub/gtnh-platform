package integration

import (
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
)

// TestGateway_ThermalPowerChain places the complete bounded thermal-to-electric
// fixture and verifies machine state transitions through Gateway snapshots.
func TestGateway_ThermalPowerChain(t *testing.T) {
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()

	const playerID = uint64(1101)
	// CAS placement requires the target chunk to exist in ChunkStore. Request
	// the fixture chunks explicitly because Gateway's automatic spawn request
	// covers only the player's initial position.
	for _, chunkX := range []int32{700 >> 5, 707 >> 5} {
		if err := c.RequestChunk(playerID, chunkX, 120>>5, 700>>5); err != nil {
			t.Fatalf("request fixture chunk x=%d: %v", chunkX, err)
		}
	}
	c.WaitForChunkGeneration(6 * time.Second)

	const heatGenerator uint16 = 0xE002
	const heatBoiler uint16 = 0xE601
	const fluidPipe uint16 = 0xF800
	const turbine uint16 = 0xE42C
	const batteryBuffer uint16 = 0xEA00
	const batteryItem uint16 = 0xEE14
	const compressor uint16 = 0xE404
	const coal uint16 = 0x7802
	const ironIngot uint16 = 0x6001
	const cableTin uint16 = 0xF400

	positions := []struct {
		x, y, z int32
		id      uint16
	}{
		{700, 120, 700, heatGenerator},
		{701, 120, 700, heatBoiler},
		{701, 121, 700, fluidPipe},
		{702, 121, 700, fluidPipe},
		{703, 121, 700, turbine},
		{704, 121, 700, cableTin},
		{705, 121, 700, batteryBuffer},
		{706, 121, 700, cableTin},
		{707, 121, 700, compressor},
	}

	for i, p := range positions {
		reqID := uint32(10000 + i)
		if err := c.SendCtrl(testutil.MsgSetBlockAction,
			testutil.BuildSetBlockActionWithOptions(playerID, p.x, p.y+1, p.z, 0, p.id,
				testutil.SetBlockActionOptions{RequestID: reqID, Face: 0, HeldItem: p.id})); err != nil {
			t.Fatalf("place block %d: %v", p.id, err)
		}
		ackData, err := c.WaitForBlockAck(reqID, Protocol.BlockAckStatusACCEPTED, 5*time.Second)
		if err != nil {
			t.Fatalf("place block %d ACK: %v", p.id, err)
		}
		ack := Protocol.GetRootAsBlockAck(ackData, 0)
		if ack == nil || ack.BlockId() != p.id {
			t.Fatalf("place block %d ACK returned block %d", p.id, ack.BlockId())
		}
	}

	// Opening machine windows is part of the real client lifecycle. It
	// rehydrates the ECS inventory/session after reconnects; do it before
	// inserting items, as the GUI workaround does.
	open := func(x, y, z int32) {
		if err := c.SendCtrl(testutil.MsgMachineOpenReq,
			testutil.BuildContainerOpenReq(playerID, x, y, z)); err != nil {
			t.Fatalf("open machine at (%d,%d,%d): %v", x, y, z, err)
		}
		time.Sleep(100 * time.Millisecond)
	}
	for _, p := range []struct{ x, y, z int32 }{
		{700, 120, 700}, {701, 120, 700}, {703, 121, 700},
		{705, 121, 700}, {707, 121, 700},
	} {
		open(p.x, p.y, p.z)
	}

	// Fuel the heat source and provide a compressor input using the existing
	// machine-slot protocol. player_slot=255 means the item comes from cursor.
	put := func(x, y, z int32, slot uint16, item uint16, count uint8) {
		if err := c.SendCtrl(testutil.MsgSetMachineSlot,
			testutil.BuildSetMachineSlotReq(playerID, x, y, z, slot, item, count, 0, 255)); err != nil {
			t.Fatalf("insert item %d: %v", item, err)
		}
		if _, err := c.ExpectMsgType(testutil.MsgSetMachineSlotResp, 5*time.Second); err != nil {
			t.Fatalf("insert item %d response: %v", item, err)
		}
	}
	put(700, 120, 700, 0, coal, 64)
	put(705, 121, 700, 0, batteryItem, 1)
	put(707, 121, 700, 0, ironIngot, 1)

	seenSteam := false
	seenTurbineEU := false
	seenBatteryCharge := false
	seenCompressorProgress := false
	deadline := time.Now().Add(35 * time.Second)
	for time.Now().Before(deadline) &&
		!(seenSteam && seenTurbineEU && seenBatteryCharge && seenCompressorProgress) {
		msgType, data, readErr := c.ReadCtrl(500 * time.Millisecond)
		if readErr != nil {
			continue
		}
		if msgType != testutil.MsgBlockEntityUpdate {
			continue
		}
		update := Protocol.GetRootAsBlockEntityUpdate(data, 0)
		var pos Protocol.Vec3i
		if update == nil || update.Pos(&pos) == nil {
			continue
		}
		switch {
		case pos.X() == 701 && pos.Y() == 120 && pos.Z() == 700:
			seenSteam = update.SteamCurrent() > 0
		case pos.X() == 703 && pos.Y() == 121 && pos.Z() == 700:
			seenTurbineEU = update.Energy() > 0
		case pos.X() == 705 && pos.Y() == 121 && pos.Z() == 700:
			seenBatteryCharge = update.Energy() > 0
		case pos.X() == 707 && pos.Y() == 121 && pos.Z() == 700:
			seenCompressorProgress = update.Progress() > 0
		}
	}

	if !seenSteam {
		t.Fatalf("heat boiler never exposed positive steam")
	}
	if !seenTurbineEU {
		t.Fatalf("steam turbine never exposed positive EU")
	}
	if !seenBatteryCharge {
		t.Fatalf("battery buffer never exposed positive EU")
	}
	if !seenCompressorProgress {
		t.Fatalf("compressor never advanced with 32 EU/t recipe")
	}
}
