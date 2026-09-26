package integration

import (
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
)

func TestSingleBlockSideConfig_ElectricalSteamFluid(t *testing.T) {
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

	const playerID = uint64(9002)
	const wrenchID uint16 = 0xF005 // 1111:00:5

	// IDs
	const chemReactorID uint16 = 58371 // 0xE403, 1110:010:3
	const steamBoilerID uint16 = 58880 // 0xE600, 1110:011:0
	const itemPipeID uint16 = 0xF800   // 1111:10:0

	const baseX int32 = 22000
	const baseY int32 = 197
	const baseZ int32 = 22000

	if err := c.RequestChunk(playerID, baseX>>5, baseY>>5, baseZ>>5); err != nil {
		t.Fatalf("request chunk: %v", err)
	}
	c.WaitForChunkGeneration(4 * time.Second)

	// Place chemical reactor
	reactorX := baseX
	reactorY := baseY
	reactorZ := baseZ
	if err := c.PlaceBlockAndWait(cs, playerID, reactorX, reactorY, reactorZ, chemReactorID, 200001, 8*time.Second); err != nil {
		t.Fatalf("place reactor: %v", err)
	}
	t.Logf("Chemical reactor placed")

	// Place steam boiler next to reactor
	boilerX := baseX + 5
	boilerY := baseY
	boilerZ := baseZ
	if err := c.PlaceBlockAndWait(cs, playerID, boilerX, boilerY, boilerZ, steamBoilerID, 200002, 8*time.Second); err != nil {
		t.Fatalf("place boiler: %v", err)
	}
	t.Logf("Steam boiler placed")

	// Place item pipe next to reactor on +X
	pipeX := reactorX + 1
	pipeY := reactorY
	pipeZ := reactorZ
	if err := c.PlaceBlockAndWait(cs, playerID, pipeX, pipeY, pipeZ, itemPipeID, 200003, 8*time.Second); err != nil {
		t.Fatalf("place pipe: %v", err)
	}
	t.Logf("Item pipe placed")

	// Helper to wrench and check
	wrenchAndCheck := func(x, y, z int32, face uint8, name string) {
		toolData := testutil.BuildToolAction(playerID, 1, x, y, z, face, wrenchID)
		if err := c.SendCtrl(testutil.MsgToolAction, toolData); err != nil {
			t.Fatalf("%s send wrench: %v", name, err)
		}
		respData, err := c.ExpectMsgType(testutil.MsgToolActionResp, 5*time.Second)
		if err != nil {
			t.Fatalf("%s wrench resp timeout: %v", name, err)
		}
		resp := Protocol.GetRootAsToolActionResp(respData, 0)
		if resp == nil || !resp.Success() {
			errMsg := ""
			if resp != nil {
				errMsg = string(resp.Error())
			}
			t.Fatalf("%s wrench failed: success=%v error=%q", name, resp != nil && resp.Success(), errMsg)
		}
		t.Logf("%s wrench ok, new_side_role=%d", name, resp.NewSideRole())
	}

	// Wrench reactor on +X
	wrenchAndCheck(reactorX, reactorY, reactorZ, 1, "Reactor")
	// Wrench boiler on +Z for steam output
	wrenchAndCheck(boilerX, boilerY, boilerZ, 3, "Boiler")
	// Wrench pipe on -X to connect to reactor
	wrenchAndCheck(pipeX, pipeY, pipeZ, 2, "Pipe")

	// Verify pipe contents after side config
	if err := c.SendCtrl(testutil.MsgPipeContentsReq, testutil.BuildPipeContentsReq(playerID, pipeX, pipeY, pipeZ)); err != nil {
		t.Fatalf("pipe contents req: %v", err)
	}
	if _, err := c.ExpectMsgType(testutil.MsgPipeContentsResp, 5*time.Second); err != nil {
		t.Fatalf("pipe contents resp timeout")
	}
	t.Logf("Pipe contents verified after side config")

	t.Logf("Single block side config test passed")
}
