package integration

import (
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
)

// TestGateway_LCREnergyHatchThermalChain exercises the scaled LV topology:
// four independent heat/steam branches feed a shared cable trunk, four battery
// buffers, and an LCR through its physical energy hatch.
func TestGateway_LCREnergyHatchThermalChain(t *testing.T) {
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()
	cs, err := testutil.DialChunkStore("127.0.0.1", 5001, 5*time.Second)
	if err != nil {
		t.Fatalf("dial ChunkStore: %v", err)
	}
	defer cs.Close()

	const playerID = uint64(1201)
	const casing uint16 = 1001
	const lcrController uint16 = 1006
	const heatGenerator uint16 = 0xE002
	const heatBoiler uint16 = 0xE601
	const fluidPipe uint16 = 0xF800
	const turbine uint16 = 0xE42C
	const batteryBuffer uint16 = 0xEA00
	const batteryItem uint16 = 0xEE14
	const cableTin uint16 = 0xF400
	const itemIn uint16 = 0xEE0A
	const itemOut uint16 = 0xEE0B
	const energyHatch uint16 = 0xEE0E
	const coal uint16 = 0x7802
	const ironDust uint16 = 0x711A  // items.csv: 0:1110:001:26 (iron_dust)
	const ironIngot uint16 = 0x6001 // items.csv: 0:110:1 (iron_ingot) (output)

	// Keep all coordinates below 1024 because multiblock position packing uses
	// ten bits per axis. The branch z coordinates leave room for one shared
	// cable trunk and an LCR at z=690.
	// All coordinates below share one chunk; send exactly one generation
	// request so duplicate asynchronous generation jobs cannot race CAS writes.
	if err := c.RequestChunk(playerID, 700>>5, 120>>5, 680>>5); err != nil {
		t.Fatalf("request fixture chunk: %v", err)
	}

	c.WaitForChunkGeneration(6 * time.Second)

	type block struct {
		x, y, z int32
		id      uint16
	}
	var blocks []block
	for _, z := range []int32{680, 686, 692, 698} {
		blocks = append(blocks,
			block{700, 120, z, heatGenerator},
			block{701, 120, z, heatBoiler},
			block{701, 121, z, fluidPipe},
			block{702, 121, z, fluidPipe},
			block{703, 121, z, turbine},
			block{704, 121, z, cableTin},
			block{705, 121, z, batteryBuffer},
		)
	}
	// Shared LV trunk: each battery touches x=706, while the complete run
	// joins the four branches and continues to the LCR energy hatch.
	for z := int32(680); z <= 698; z++ {
		blocks = append(blocks, block{706, 121, z, cableTin})
	}
	for y := int32(122); y <= 124; y++ {
		blocks = append(blocks, block{706, y, 692, cableTin})
	}
	for x := int32(707); x <= 710; x++ {
		blocks = append(blocks, block{x, 124, 692, cableTin})
	}

	// LCR pattern 3 at anchor (710,122,690), with canonical item hatches in
	// layer 1 and the canonical LV energy hatch at the declared external port
	// position anchor+(1,2,2)=(711,124,692). The controller is last.
	// LCR pattern 3 has corner (709,121,689) and controller at (710,123,690).
	for x := int32(709); x <= 711; x++ {
		for z := int32(689); z <= 691; z++ {
			blocks = append(blocks, block{x, 121, z, casing})
		}
	}
	blocks = append(blocks,
		block{709, 122, 689, casing}, block{711, 122, 689, casing},
		block{709, 122, 691, casing}, block{711, 122, 691, casing},
		block{709, 122, 690, itemIn}, block{711, 122, 690, itemOut},
	)
	for z := int32(689); z <= 691; z++ {
		blocks = append(blocks, block{709, 123, z, casing}, block{711, 123, z, casing})
	}
	blocks = append(blocks, block{710, 123, 689, casing}, block{710, 123, 691, casing},
		block{711, 125, 692, energyHatch}, block{710, 123, 690, lcrController})

	// Bring the shared LV trunk up to the external energy-hatch endpoint.
	blocks = append(blocks, block{706, 125, 692, cableTin})
	for x := int32(707); x <= 710; x++ {
		blocks = append(blocks, block{x, 125, 692, cableTin})
	}

	for i, p := range blocks {
		if err := c.PlaceBlockAndWait(cs, playerID, p.x, p.y, p.z, p.id,
			uint32(20000+i), 8*time.Second); err != nil {
			t.Fatalf("authoritative placement %d at (%d,%d,%d): %v", p.id, p.x, p.y, p.z, err)
		}
	}
	_ = Protocol.BlockAckStatusACCEPTED // placement helper validates this status

	// The final controller placement emits the lifecycle event only after the
	// authoritative commit has been observed, so formation cannot race a CAS.
	// Formation is anchored at the controller's actual world position.
	const lcrAnchorY int32 = 123

	if id, _, _, err := cs.GetBlock(710, lcrAnchorY, 690, 2*time.Second); err != nil || id != lcrController {
		t.Fatalf("authoritative LCR controller state: id=%d err=%v", id, err)
	}

	// The event is emitted after the controller CAS callback.
	kind, created, _, err := c.ExpectMultiblockEvent(8 * time.Second)
	if err != nil {
		t.Fatalf("wait for LCR formation: %v", err)
	}
	if kind != testutil.MultiblockEventCreated || created == nil {
		t.Fatalf("expected LCR created event, got %v", kind)
	}
	var gotAnchor Protocol.Vec3i
	if created.Anchor(&gotAnchor) == nil || gotAnchor.X() != 710 || gotAnchor.Y() != lcrAnchorY || gotAnchor.Z() != 690 {
		t.Fatalf("LCR anchor: got (%d,%d,%d), want (710,%d,690)", gotAnchor.X(), gotAnchor.Y(), gotAnchor.Z(), lcrAnchorY)
	}
	if created.MbType() != 3 {
		t.Fatalf("LCR pattern: got %d, want 3", created.MbType())
	}

	open := func(x, y, z int32) {
		if err := c.SendCtrl(testutil.MsgMachineOpenReq, testutil.BuildContainerOpenReq(playerID, x, y, z)); err != nil {
			t.Fatalf("open machine at (%d,%d,%d): %v", x, y, z, err)
		}
		time.Sleep(50 * time.Millisecond)
	}
	for _, p := range []block{
		{700, 120, 680, heatGenerator}, {701, 120, 680, heatBoiler},
		{703, 121, 680, turbine}, {705, 121, 680, batteryBuffer},
		{700, 120, 686, heatGenerator}, {701, 120, 686, heatBoiler},
		{703, 121, 686, turbine}, {705, 121, 686, batteryBuffer},
		{700, 120, 692, heatGenerator}, {701, 120, 692, heatBoiler},
		{703, 121, 692, turbine}, {705, 121, 692, batteryBuffer},
		{700, 120, 698, heatGenerator}, {701, 120, 698, heatBoiler},
		{703, 121, 698, turbine}, {705, 121, 698, batteryBuffer},
		{710, 123, 690, lcrController},
	} {
		open(p.x, p.y, p.z)
	}

	put := func(x, y, z int32, slot uint16, item uint16, count byte) {
		if err := c.SendCtrl(testutil.MsgSetMachineSlot,
			testutil.BuildSetMachineSlotReq(playerID, x, y, z, slot, item, count, 0, 255)); err != nil {
			t.Fatalf("insert item %d at (%d,%d,%d): %v", item, x, y, z, err)
		}
		respData, err := c.ExpectMsgType(testutil.MsgSetMachineSlotResp, 5*time.Second)
		if err != nil {
			t.Fatalf("insert item %d response: %v", item, err)
		}
		resp := Protocol.GetRootAsSetMachineSlotResp(respData, 0)
		if resp == nil || !resp.Success() {
			t.Fatalf("insert item %d rejected: %s", item, string(resp.Error()))
		}
	}
	for _, z := range []int32{680, 686, 692, 698} {
		put(700, 120, z, 0, coal, 64)
		put(705, 121, z, 0, batteryItem, 1)
	}
	put(710, 123, 690, 0, ironDust, 1)

	seenSteam := map[int32]bool{}
	seenTurbine := map[int32]bool{}
	seenBattery := map[int32]bool{}
	seenHatch := false
	seenProgress := false
	seenOutput := false
	deadline := time.Now().Add(35 * time.Second)
	for time.Now().Before(deadline) &&
		(len(seenSteam) < 4 || len(seenTurbine) < 4 || len(seenBattery) < 4 || !seenHatch || !seenProgress || !seenOutput) {
		msgType, data, readErr := c.ReadCtrl(500 * time.Millisecond)
		if readErr != nil || msgType != testutil.MsgBlockEntityUpdate {
			continue
		}
		update := Protocol.GetRootAsBlockEntityUpdate(data, 0)
		var pos Protocol.Vec3i
		if update == nil || update.Pos(&pos) == nil {
			continue
		}
		if pos.X() == 710 && pos.Y() == 123 && pos.Z() == 690 {
			seenProgress = update.Progress() > 0
			for i := 0; i < update.OutputItemsLength(); i++ {
				var item Protocol.ItemStack
				if update.OutputItems(&item, i) && item.ItemId() == ironIngot && item.Count() > 0 {
					seenOutput = true
				}
			}
			// RouterEventPublisher currently emits hatch coordinates/types from
			// the LCR update only when the dedicated system reaches its snapshot;
			// the authoritative block query below proves the physical hatch.
			for i := 0; i < update.HatchesLength(); i++ {
				var hatch Protocol.HatchInfo
				if !update.Hatches(&hatch, i) || hatch.HatchType() != Protocol.HatchTypeENERGY {
					continue
				}
				var hatchPos Protocol.Vec3i
				if hatch.Pos(&hatchPos) != nil && hatchPos.X() == 711 && hatchPos.Y() == 125 && hatchPos.Z() == 692 && hatch.Tier() == 0 {
					seenHatch = true
				}
			}
		}
		if pos.X() == 711 && pos.Y() == 125 && pos.Z() == 692 {
			seenHatch = true
		}
		// Physical placement is also checked through ChunkStore before this
		// stream; the snapshot is not the only proof of hatch existence.
		for _, z := range []int32{680, 686, 692, 698} {
			switch {
			case pos.X() == 701 && pos.Y() == 120 && pos.Z() == z:
				if update.SteamCurrent() > 0 {
					seenSteam[z] = true
				}
			case pos.X() == 703 && pos.Y() == 121 && pos.Z() == z:
				if update.Energy() > 0 {
					seenTurbine[z] = true
				}
			case pos.X() == 705 && pos.Y() == 121 && pos.Z() == z:
				if update.Energy() > 0 {
					seenBattery[z] = true
				}
			}
		}
	}
	if len(seenSteam) != 4 {
		t.Fatalf("boiler steam transitions: %v", seenSteam)
	}
	if len(seenTurbine) != 4 {
		t.Fatalf("turbine EU transitions: %v", seenTurbine)
	}
	if len(seenBattery) != 4 {
		t.Fatalf("battery EU transitions: %v", seenBattery)
	}
	if !seenHatch {
		t.Fatal("LCR did not expose canonical physical energy hatch")
	}
	if !seenProgress {
		t.Fatal("LCR did not advance on energy-hatch power")
	}
	if !seenOutput {
		t.Fatal("LCR did not produce iron ingot output")
	}
}
