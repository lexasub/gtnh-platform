package integration

import (
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
)

func TestPipeWrench_MachineSideConfig(t *testing.T) {
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

	const playerID = uint64(7777)
	const wrenchID uint16 = 0xF005 // wrench = 1111:00:5
	// Use a fresh area far from other fixtures
	const baseX int32 = 21000
	const baseY int32 = 197
	const baseZ int32 = 21000

	if err := c.RequestChunk(playerID, baseX>>5, baseY>>5, baseZ>>5); err != nil {
		t.Fatalf("request chunk: %v", err)
	}
	c.WaitForChunkGeneration(4 * time.Second)

	// Place an item pipe
	pipeID := uint16(0xF800) // item pipe
	pipeX := baseX
	pipeY := baseY
	pipeZ := baseZ

	if err := c.PlaceBlockAndWait(cs, playerID, pipeX, pipeY, pipeZ, pipeID, 90001, 8*time.Second); err != nil {
		t.Fatalf("place pipe: %v", err)
	}

	// Wrench the pipe on +X face to cycle side config
	face := uint8(1)                                                                       // +X
	toolData := testutil.BuildToolAction(playerID, 1, pipeX, pipeY, pipeZ, face, wrenchID) // WRENCH_CYCLE =1
	if err := c.SendCtrl(testutil.MsgToolAction, toolData); err != nil {
		t.Fatalf("send wrench action: %v", err)
	}
	// Expect ToolActionResp
	respData, err := c.ExpectMsgType(testutil.MsgToolActionResp, 5*time.Second)
	if err != nil {
		t.Fatalf("tool action resp: %v", err)
	}
	resp := Protocol.GetRootAsToolActionResp(respData, 0)
	if resp == nil || !resp.Success() {
		errMsg := ""
		if resp != nil {
			errMsg = string(resp.Error())
		}
		t.Fatalf("wrench failed: success=%v error=%s", resp != nil && resp.Success(), errMsg)
	}
	t.Logf("Wrench succeeded, new_side_role=%d", resp.NewSideRole())

	// Verify pipe connectivity via PipeContentsReq
	if err := c.SendCtrl(48, testutil.BuildPipeContentsReq(playerID, pipeX, pipeY, pipeZ)); err != nil {
		t.Fatalf("pipe contents req: %v", err)
	}
	// Expect PipeContentsResp
	if _, err := c.ExpectMsgType(49, 5*time.Second); err != nil {
		t.Fatalf("pipe contents resp: %v", err)
	}
	t.Logf("Pipe contents response received, side config applied")
}
