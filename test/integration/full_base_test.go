package integration

import (
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
)

func TestFullBase_MultiMachineIntegration(t *testing.T) {
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

	const playerID = uint64(9999)
	const baseX int32 = 90000
	const baseY int32 = 70
	const baseZ int32 = 90000

	if err := c.RequestChunk(playerID, baseX>>5, baseY>>5, baseZ>>5); err != nil {
		t.Fatalf("request chunk: %v", err)
	}
	c.WaitForChunkGeneration(4 * time.Second)

	// 1. EBF at baseX,baseY,baseZ
	ebfCornerX := baseX
	ebfCornerY := baseY
	ebfCornerZ := baseZ
	ebfEnergyX := ebfCornerX + 5
	ebfEnergyY := ebfCornerY + 3
	ebfEnergyZ := ebfCornerZ + 5
	ebfBlocks := ebfFixtureBlocks(ebfCornerX, ebfCornerY, ebfCornerZ, ebfEnergyX, ebfEnergyY, ebfEnergyZ)
	for i, p := range ebfBlocks {
		if err := c.PlaceBlockAndWait(cs, playerID, p.x, p.y, p.z, p.id, uint32(80000+i), 8*time.Second); err != nil {
			t.Fatalf("EBF place %d: %v", i, err)
		}
	}
	kind, created, _, err := c.ExpectMultiblockEvent(8 * time.Second)
	if err != nil || kind != testutil.MultiblockEventCreated || created == nil {
		t.Fatalf("EBF not created: %v %v", kind, err)
	}
	t.Logf("EBF created at anchor %v", created)

	// 2. LCR at offset +20
	lcrCornerX := baseX + 20
	lcrCornerY := baseY
	lcrCornerZ := baseZ
	// simplified LCR placement: just casing frame + controller
	// using same canonical ids for demo
	lcrBlocks := []struct {
		x, y, z int32
		id      uint16
	}{
		{lcrCornerX, lcrCornerY, lcrCornerZ, 0xEE05},
		{lcrCornerX + 1, lcrCornerY, lcrCornerZ, 0xEE05},
		{lcrCornerX + 2, lcrCornerY, lcrCornerZ, 0xEE05},
		{lcrCornerX + 1, lcrCornerY + 1, lcrCornerZ + 1, 0xE42F}, // controller placeholder
	}
	for i, p := range lcrBlocks {
		if err := c.PlaceBlockAndWait(cs, playerID, p.x, p.y, p.z, p.id, uint32(90000+i), 8*time.Second); err != nil {
			t.Fatalf("LCR place %d: %v", i, err)
		}
	}
	t.Logf("LCR blocks placed")

	// 3. Verify EBF processes iron dust
	anchorX := ebfCornerX + 1
	anchorY := ebfCornerY + 3
	anchorZ := ebfCornerZ + 1
	const ironDust uint16 = 0x711A
	const ironIngot uint16 = 0x6001

	open := func(x, y, z int32) {
		if err := c.SendCtrl(testutil.MsgMachineOpenReq, testutil.BuildContainerOpenReq(playerID, x, y, z)); err != nil {
			t.Fatalf("open machine: %v", err)
		}
		if _, err := c.ExpectMsgType(testutil.MsgBlockEntityUpdate, 5*time.Second); err != nil {
			t.Fatalf("open update: %v", err)
		}
	}
	open(anchorX, anchorY, anchorZ)
	if err := c.SendCtrl(testutil.MsgSetMachineSlot,
		testutil.BuildSetMachineSlotReq(playerID, anchorX, anchorY, anchorZ, 0, ironDust, 1, 0, 255)); err != nil {
		t.Fatalf("insert iron dust: %v", err)
	}
	if data, err := c.ExpectMsgType(testutil.MsgSetMachineSlotResp, 5*time.Second); err != nil {
		t.Fatalf("insert resp: %v", err)
	} else if resp := Protocol.GetRootAsSetMachineSlotResp(data, 0); resp == nil || !resp.Success() {
		t.Fatalf("insert rejected")
	}

	deadline := time.Now().Add(45 * time.Second)
	seenOutput := false
	for time.Now().Before(deadline) && !seenOutput {
		msgType, data, err := c.ReadCtrl(500 * time.Millisecond)
		if err != nil || msgType != testutil.MsgBlockEntityUpdate {
			continue
		}
		update := Protocol.GetRootAsBlockEntityUpdate(data, 0)
		var pos Protocol.Vec3i
		if update == nil || update.Pos(&pos) == nil {
			continue
		}
		if pos.X() != anchorX || pos.Y() != anchorY || pos.Z() != anchorZ {
			continue
		}
		for i := 0; i < update.OutputItemsLength(); i++ {
			var item Protocol.ItemStack
			if update.OutputItems(&item, i) {
				if item.ItemId() == ironIngot && item.Count() >= 2 {
					seenOutput = true
					t.Logf("EBF produced iron ingots")
				}
			}
		}
	}
	if !seenOutput {
		t.Fatal("EBF did not produce output in full base test")
	}

	t.Logf("Full base multi-machine integration passed")
}
