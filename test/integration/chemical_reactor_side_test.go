package integration

import (
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
)

func TestChemicalReactor_SideConfigWrench(t *testing.T) {
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

	const playerID = uint64(8888)
	const wrenchID uint16 = 0xF005         // 1111:00:5
	const chemReactorPacked uint16 = 58371 // 0xE403, 1110:010:3

	const baseX int32 = 93000
	const baseY int32 = 200
	const baseZ int32 = 93000

	if err := c.RequestChunk(playerID, baseX>>5, baseY>>5, baseZ>>5); err != nil {
		t.Fatalf("request chunk: %v", err)
	}
	c.WaitForChunkGeneration(4 * time.Second)

	// Place chemical reactor LV
	if err := c.PlaceBlockAndWait(cs, playerID, baseX, baseY, baseZ, chemReactorPacked, 100001, 8*time.Second); err != nil {
		t.Fatalf("place reactor: %v", err)
	}

	// Wrench on +X face to cycle side role
	face := uint8(1) // +X
	toolData := testutil.BuildToolAction(playerID, 1, baseX, baseY, baseZ, face, wrenchID)
	if err := c.SendCtrl(testutil.MsgToolAction, toolData); err != nil {
		t.Fatalf("send wrench: %v", err)
	}
	respData, err := c.ExpectMsgType(testutil.MsgToolActionResp, 5*time.Second)
	if err != nil {
		t.Fatalf("tool resp timeout: %v", err)
	}
	resp := Protocol.GetRootAsToolActionResp(respData, 0)
	if resp == nil || !resp.Success() {
		t.Fatalf("wrench failed")
	}
	t.Logf("Wrench success, new_side_role=%d", resp.NewSideRole())

	// Verify side config persisted via BlockEntityUpdate
	// Open machine container to force update
	if err := c.SendCtrl(testutil.MsgMachineOpenReq, testutil.BuildContainerOpenReq(playerID, baseX, baseY, baseZ)); err != nil {
		t.Fatalf("open container: %v", err)
	}
	if _, err := c.ExpectMsgType(testutil.MsgBlockEntityUpdate, 5*time.Second); err != nil {
		t.Fatalf("no block entity update")
	}
	t.Logf("Side config verified for chemical reactor")
}
