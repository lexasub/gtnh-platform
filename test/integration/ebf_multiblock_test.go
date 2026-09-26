package integration

import (
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
)

// Canonical EBF block ids (pattern "ebf", pattern_id 1).
const (
	canonicalCasing  uint16 = 0xEE05 // 1110:111:5
	canonicalCoil    uint16 = 0xEE06 // 1110:111:6 (Kanthal)
	canonicalCtrl    uint16 = 0xE42F // 1110:010:47
	canonicalItemIn  uint16 = 0xEE0A // 1110:111:10
	canonicalItemOut uint16 = 0xEE0B // 1110:111:11
	canonicalEnergy  uint16 = 0xEE0E // 1110:111:14
	canonicalCable   uint16 = 0xF400 // 1111:01:0
	creativeGen      uint16 = 0xE800
)

type fixtureBlock struct {
	x, y, z int32
	id      uint16
}

// ebfFixtureBlocks builds the physical EBF layout: 3x3x3 casing frame with two
// Kanthal coil layers, ITEM_IN/ITEM_OUT hatches on layer 1 and the controller
// placed last. When energy positions are non-negative, the fixture also links a
// creative EU source through one tin cable to the physical energy hatch.
func ebfFixtureBlocks(cornerX, cornerY, cornerZ, energyX, energyY, energyZ int32) []fixtureBlock {
	var blocks []fixtureBlock
	for x := int32(0); x < 3; x++ {
		for z := int32(0); z < 3; z++ {
			blocks = append(blocks, fixtureBlock{cornerX + x, cornerY, cornerZ + z, canonicalCasing})
		}
	}
	blocks = append(blocks,
		fixtureBlock{cornerX, cornerY + 1, cornerZ, canonicalCasing},
		fixtureBlock{cornerX + 2, cornerY + 1, cornerZ, canonicalCasing},
		fixtureBlock{cornerX, cornerY + 1, cornerZ + 2, canonicalCasing},
		fixtureBlock{cornerX + 2, cornerY + 1, cornerZ + 2, canonicalCasing},
		fixtureBlock{cornerX + 1, cornerY + 1, cornerZ + 1, canonicalCoil},
		fixtureBlock{cornerX, cornerY + 2, cornerZ, canonicalCasing},
		fixtureBlock{cornerX + 2, cornerY + 2, cornerZ, canonicalCasing},
		fixtureBlock{cornerX, cornerY + 2, cornerZ + 2, canonicalCasing},
		fixtureBlock{cornerX + 2, cornerY + 2, cornerZ + 2, canonicalCasing},
		fixtureBlock{cornerX + 1, cornerY + 2, cornerZ + 1, canonicalCoil})
	for x := int32(0); x < 3; x++ {
		for z := int32(0); z < 3; z++ {
			if x != 1 || z != 1 {
				blocks = append(blocks, fixtureBlock{cornerX + x, cornerY + 3, cornerZ + z, canonicalCasing})
			}
		}
	}
	if energyX >= 0 && energyY >= 0 && energyZ >= 0 {
		blocks = append(blocks,
			fixtureBlock{energyX, energyY, energyZ, canonicalEnergy},
			fixtureBlock{energyX + 1, energyY, energyZ, canonicalCable},
			fixtureBlock{energyX + 2, energyY, energyZ, creativeGen})
	}
	blocks = append(blocks,
		fixtureBlock{cornerX, cornerY + 1, cornerZ + 1, canonicalItemIn},
		fixtureBlock{cornerX + 2, cornerY + 1, cornerZ + 1, canonicalItemOut},
		fixtureBlock{cornerX + 1, cornerY + 3, cornerZ + 1, canonicalCtrl})
	return blocks
}

func TestGateway_EBFMultiblockLifecycle(t *testing.T) {
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()

	const playerID = uint64(1003)
	cs, err := testutil.DialChunkStore("127.0.0.1", 5001, 5*time.Second)
	if err != nil {
		t.Fatalf("dial ChunkStore: %v", err)
	}
	defer cs.Close()

	// Keep the fixture above generated terrain and away from other tests.
	corner := [3]int32{511, 197, 511}
	anchor := [3]int32{512, 200, 512}

	if err := c.RequestChunk(playerID, corner[0]>>5, corner[1]>>5, corner[2]>>5); err != nil {
		t.Fatalf("request EBF fixture chunk: %v", err)
	}
	c.WaitForChunkGeneration(4 * time.Second)

	placements := ebfFixtureBlocks(corner[0], corner[1], corner[2], -1, -1, -1)
	for i, p := range placements {
		if err := c.PlaceBlockAndWait(cs, playerID, p.x, p.y, p.z, p.id,
			uint32(30000+i), 8*time.Second); err != nil {
			t.Fatalf("place EBF block %d at (%d,%d,%d): %v", p.id, p.x, p.y, p.z, err)
		}
	}

	kind, created, _, err := c.ExpectMultiblockEvent(5 * time.Second)
	if err != nil {
		t.Fatalf("wait for EBF created event: %v", err)
	}
	if kind != testutil.MultiblockEventCreated || created == nil {
		t.Fatalf("expected created event, got %v", kind)
	}
	var gotAnchor Protocol.Vec3i
	if created.Anchor(&gotAnchor) == nil || gotAnchor.X() != anchor[0] || gotAnchor.Y() != anchor[1] || gotAnchor.Z() != anchor[2] {
		t.Fatalf("created anchor: got (%d,%d,%d), want (%d,%d,%d)", gotAnchor.X(), gotAnchor.Y(), gotAnchor.Z(), anchor[0], anchor[1], anchor[2])
	}
	if created.MbType() != 1 {
		t.Fatalf("created type: got %d, want EBF pattern 1", created.MbType())
	}

	teardownRequest := uint32(len(placements) + 1000)
	if err := c.SendCtrl(testutil.MsgSetBlockAction, testutil.BuildBreakBlockActionWithOptions(playerID, anchor[0], anchor[1], anchor[2], canonicalCtrl, testutil.SetBlockActionOptions{RequestID: teardownRequest})); err != nil {
		t.Fatalf("break EBF anchor: %v", err)
	}
	ackData, err := c.WaitForBlockAck(teardownRequest, Protocol.BlockAckStatusACCEPTED, 5*time.Second)
	if err != nil {
		t.Fatalf("anchor break ack: %v", err)
	}
	ack := Protocol.GetRootAsBlockAck(ackData, 0)
	if ack.BlockId() != 0 {
		t.Fatalf("anchor teardown block id: got %d, want air", ack.BlockId())
	}
	kind, _, destroyed, err := c.ExpectMultiblockEvent(5 * time.Second)
	if err != nil {
		t.Fatalf("wait for EBF destroyed event: %v", err)
	}
	if kind != testutil.MultiblockEventDestroyed || destroyed == nil {
		t.Fatalf("expected destroyed event, got %v", kind)
	}
}

// TestGateway_EBFProcessesIronDust exercises the complete canonical EBF path:
// physical multiblock formation, EU delivery through the energy hatch, and
// iron_dust -> 2 iron_ingot processing through the item hatches.
func TestGateway_EBFProcessesIronDust(t *testing.T) {
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

	const playerID = uint64(1401)
	const ironDust uint16 = 0x711A  // 0:1110:001:26
	const ironIngot uint16 = 0x6001 // 0:110:1

	const cornerX int32 = 820
	const cornerY int32 = 120
	const cornerZ int32 = 820
	const anchorX int32 = cornerX + 1
	const anchorY int32 = cornerY + 3
	const anchorZ int32 = cornerZ + 1
	// Pattern hatch offsets are controller-relative: energy at (+1, +3, +3).
	const energyX int32 = anchorX + 1
	const energyY int32 = anchorY + 3
	const energyZ int32 = anchorZ + 3

	if err := c.RequestChunk(playerID, cornerX>>5, cornerY>>5, cornerZ>>5); err != nil {
		t.Fatalf("request EBF fixture chunk: %v", err)
	}
	c.WaitForChunkGeneration(4 * time.Second)

	type block = fixtureBlock
	blocks := ebfFixtureBlocks(cornerX, cornerY, cornerZ, energyX, energyY, energyZ)

	for i, p := range blocks {
		if err := c.PlaceBlockAndWait(cs, playerID, p.x, p.y, p.z, p.id,
			uint32(40000+i), 8*time.Second); err != nil {
			t.Fatalf("place EBF block %d at (%d,%d,%d): %v", p.id, p.x, p.y, p.z, err)
		}
	}
	kind, created, _, err := c.ExpectMultiblockEvent(8 * time.Second)
	if err != nil || kind != testutil.MultiblockEventCreated || created == nil {
		t.Fatalf("wait for EBF formation: kind=%v err=%v", kind, err)
	}
	var createdAnchor Protocol.Vec3i
	if created.Anchor(&createdAnchor) == nil || createdAnchor.X() != anchorX || createdAnchor.Y() != anchorY || createdAnchor.Z() != anchorZ {
		t.Fatalf("EBF anchor: got (%d,%d,%d), want (%d,%d,%d)", createdAnchor.X(), createdAnchor.Y(), createdAnchor.Z(), anchorX, anchorY, anchorZ)
	}
	if created.MbType() != 1 {
		t.Fatalf("EBF pattern: got %d, want 1", created.MbType())
	}
	if id, _, _, err := cs.GetBlock(energyX, energyY, energyZ, 3*time.Second); err != nil || id != canonicalEnergy {
		t.Fatalf("energy hatch authoritative state: id=%d err=%v", id, err)
	}

	open := func(x, y, z int32) {
		if err := c.SendCtrl(testutil.MsgMachineOpenReq, testutil.BuildContainerOpenReq(playerID, x, y, z)); err != nil {
			t.Fatalf("open machine at (%d,%d,%d): %v", x, y, z, err)
		}
		if _, err := c.ExpectMsgType(testutil.MsgBlockEntityUpdate, 5*time.Second); err != nil {
			t.Fatalf("open update at (%d,%d,%d): %v", x, y, z, err)
		}
	}
	open(anchorX, anchorY, anchorZ)
	if err := c.SendCtrl(testutil.MsgSetMachineSlot,
		testutil.BuildSetMachineSlotReq(playerID, anchorX, anchorY, anchorZ, 0, ironDust, 1, 0, 255)); err != nil {
		t.Fatalf("insert iron dust: %v", err)
	}
	if data, err := c.ExpectMsgType(testutil.MsgSetMachineSlotResp, 5*time.Second); err != nil {
		t.Fatalf("insert response: %v", err)
	} else if resp := Protocol.GetRootAsSetMachineSlotResp(data, 0); resp == nil || !resp.Success() {
		t.Fatalf("insert iron dust rejected")
	}

	deadline := time.Now().Add(30 * time.Second)
	seenProgress := false
	seenEU := false
	seenOutput := false
	maxProgress := float32(0)
	maxEnergy := uint32(0)
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
		seenProgress = seenProgress || update.Progress() > 0
		seenEU = seenEU || update.Energy() > 0
		if update.Progress() > maxProgress {
			maxProgress = update.Progress()
		}
		if update.Energy() > maxEnergy {
			maxEnergy = update.Energy()
		}
		for i := 0; i < update.OutputItemsLength(); i++ {
			var item Protocol.ItemStack
			if update.OutputItems(&item, i) {
				t.Logf("EBF output item %d: id=%d count=%d", i, item.ItemId(), item.Count())
				if item.ItemId() == ironIngot && item.Count() >= 2 {
					seenOutput = true
				}
			}
		}
	}
	t.Logf("EBF poll done: maxProgress=%.3f maxEnergy=%d seenEU=%v seenOutput=%v", maxProgress, maxEnergy, seenEU, seenOutput)
	if !seenEU {
		t.Fatal("EBF did not expose EU state from physical energy hatch")
	}
	if !seenProgress {
		t.Fatal("EBF did not advance progress")
	}
	if !seenOutput {
		t.Fatal("EBF did not produce two iron ingots")
	}
}
