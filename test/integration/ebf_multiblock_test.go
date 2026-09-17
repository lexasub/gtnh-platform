package integration

import (
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
)

func TestGateway_EBFMultiblockLifecycle(t *testing.T) {
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()

	const playerID = uint64(1003)
	const casing uint16 = 1001
	const coil uint16 = 1002
	const controller uint16 = 1003
	const itemIn uint16 = 0xEA00
	const itemOut uint16 = 0xEA01
	// Keep the fixture above generated terrain and away from other tests.
	corner := [3]int32{511, 197, 511}
	anchor := [3]int32{512, 200, 512}

	var placements []struct {
		x, y, z int32
		id      uint16
	}
	for x := int32(0); x < 3; x++ {
		for z := int32(0); z < 3; z++ {
			placements = append(placements, struct {
				x, y, z int32
				id      uint16
			}{corner[0] + x, corner[1], corner[2] + z, casing})
		}
	}
	// Layer 1: casing corners, item hatches, and the center coil.
	placements = append(placements,
		struct {
			x, y, z int32
			id      uint16
		}{corner[0], corner[1] + 1, corner[2], casing},
		struct {
			x, y, z int32
			id      uint16
		}{corner[0] + 2, corner[1] + 1, corner[2], casing},
		struct {
			x, y, z int32
			id      uint16
		}{corner[0], corner[1] + 1, corner[2] + 2, casing},
		struct {
			x, y, z int32
			id      uint16
		}{corner[0] + 2, corner[1] + 1, corner[2] + 2, casing},
		struct {
			x, y, z int32
			id      uint16
		}{corner[0], corner[1] + 1, corner[2] + 1, itemIn},
		struct {
			x, y, z int32
			id      uint16
		}{corner[0] + 2, corner[1] + 1, corner[2] + 1, itemOut},
		struct {
			x, y, z int32
			id      uint16
		}{corner[0] + 1, corner[1] + 1, corner[2] + 1, coil},
	)
	// Layer 2: casing corners and the second center coil.
	placements = append(placements,
		struct {
			x, y, z int32
			id      uint16
		}{corner[0], corner[1] + 2, corner[2], casing},
		struct {
			x, y, z int32
			id      uint16
		}{corner[0] + 2, corner[1] + 2, corner[2], casing},
		struct {
			x, y, z int32
			id      uint16
		}{corner[0], corner[1] + 2, corner[2] + 2, casing},
		struct {
			x, y, z int32
			id      uint16
		}{corner[0] + 2, corner[1] + 2, corner[2] + 2, casing},
		struct {
			x, y, z int32
			id      uint16
		}{corner[0] + 1, corner[1] + 2, corner[2] + 1, coil},
	)
	// Layer 3 is a casing ring; the controller is deliberately last.
	for x := int32(0); x < 3; x++ {
		for z := int32(0); z < 3; z++ {
			if x == 1 && z == 1 {
				continue
			}
			placements = append(placements, struct {
				x, y, z int32
				id      uint16
			}{corner[0] + x, corner[1] + 3, corner[2] + z, casing})
		}
	}
	placements = append(placements, struct {
		x, y, z int32
		id      uint16
	}{anchor[0], anchor[1], anchor[2], controller})

	for requestID, p := range placements {
		req := uint32(requestID + 1)
		// RIGHT_MOUSE_CLICK resolves the placement adjacent to the targeted block.
		// Face=DOWN decrements Y, so target one block above the desired cell.
		targetY := p.y + 1
		if err := c.SendCtrl(testutil.MsgSetBlockAction, testutil.BuildSetBlockActionWithOptions(playerID, p.x, targetY, p.z, 0, p.id, testutil.SetBlockActionOptions{RequestID: req, Face: 0, HeldItem: p.id})); err != nil {
			t.Fatalf("place %d (%d,%d,%d): %v", req, p.x, p.y, p.z, err)
		}
		if _, err := c.WaitForBlockAck(req, Protocol.BlockAckStatusACCEPTED, 5*time.Second); err != nil {
			t.Fatalf("ack %d (%d,%d,%d): %v", req, p.x, p.y, p.z, err)
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
	if err := c.SendCtrl(testutil.MsgSetBlockAction, testutil.BuildBreakBlockActionWithOptions(playerID, anchor[0], anchor[1], anchor[2], controller, testutil.SetBlockActionOptions{RequestID: teardownRequest})); err != nil {
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
	const casing uint16 = 0xEE05      // 1110:111:5
	const coil uint16 = 0xEE06        // 1110:111:6 (Kanthal)
	const controller uint16 = 0xE42F  // 1110:010:47
	const itemIn uint16 = 0xEE0A      // 1110:111:10
	const itemOut uint16 = 0xEE0B     // 1110:111:11
	const energyHatch uint16 = 0xEE0E // 1110:111:14
	const creativeGenerator uint16 = 0xE800
	const cable uint16 = 0xF400     // 1111:01:0
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

	type block struct {
		x, y, z int32
		id      uint16
	}
	var blocks []block
	for x := int32(0); x < 3; x++ {
		for z := int32(0); z < 3; z++ {
			blocks = append(blocks, block{cornerX + x, cornerY, cornerZ + z, casing})
		}
	}
	blocks = append(blocks,
		block{cornerX, cornerY + 1, cornerZ, casing},
		block{cornerX + 2, cornerY + 1, cornerZ, casing},
		block{cornerX, cornerY + 1, cornerZ + 2, casing},
		block{cornerX + 2, cornerY + 1, cornerZ + 2, casing},
		block{cornerX + 1, cornerY + 1, cornerZ + 1, coil},
		block{cornerX, cornerY + 2, cornerZ, casing},
		block{cornerX + 2, cornerY + 2, cornerZ, casing},
		block{cornerX, cornerY + 2, cornerZ + 2, casing},
		block{cornerX + 2, cornerY + 2, cornerZ + 2, casing},
		block{cornerX + 1, cornerY + 2, cornerZ + 1, coil})
	for x := int32(0); x < 3; x++ {
		for z := int32(0); z < 3; z++ {
			if x != 1 || z != 1 {
				blocks = append(blocks, block{cornerX + x, cornerY + 3, cornerZ + z, casing})
			}
		}
	}
	blocks = append(blocks,
		block{cornerX, cornerY + 1, cornerZ + 1, itemIn},
		block{cornerX + 2, cornerY + 1, cornerZ + 1, itemOut},
		block{energyX, energyY, energyZ, energyHatch},
		block{anchorX, anchorY, anchorZ, controller})
	// A creative EU source and one cable touch the physical hatch endpoint.
	blocks = append(blocks,
		block{energyX + 1, energyY, energyZ, cable},
		block{energyX + 2, energyY, energyZ, creativeGenerator})

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
	if id, _, _, err := cs.GetBlock(energyX, energyY, energyZ, 3*time.Second); err != nil || id != energyHatch {
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
		for i := 0; i < update.OutputItemsLength(); i++ {
			var item Protocol.ItemStack
			if update.OutputItems(&item, i) && item.ItemId() == ironIngot && item.Count() >= 2 {
				seenOutput = true
			}
		}
	}
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
