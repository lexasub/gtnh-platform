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
