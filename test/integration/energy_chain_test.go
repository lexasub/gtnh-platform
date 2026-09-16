package integration

import (
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
)

// TestGateway_ThermalPowerPrerequisites is a placement probe for the planned
// thermal-to-electric chain. It intentionally skips state assertions until the
// production turbine and rechargeable-cell contracts are complete.
func TestGateway_ThermalPowerPrerequisites(t *testing.T) {
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()

	const playerID = uint64(1101)
	const heatGenerator uint16 = 0xE002 // 1110:000:2
	const heatBoiler uint16 = 0xE601    // 1110:011:1
	const heatPipe uint16 = 0xF804      // 1111:10:4
	const fluidPipe uint16 = 0xF800     // 1111:10:0
	const turbine uint16 = 0xE42C       // 1110:010:44
	const batteryBuffer uint16 = 0xE940 // 1110:101:0
	const compressor uint16 = 0xE404   // 1110:010:4

	positions := []struct {
		x, y, z int32
		id      uint16
	}{
		{700, 120, 700, heatGenerator},
		{701, 120, 700, heatPipe},
		{702, 120, 700, heatBoiler},
		{702, 121, 700, fluidPipe},
		{703, 121, 700, fluidPipe},
		{704, 121, 700, turbine},
		{706, 121, 700, batteryBuffer},
		{708, 121, 700, compressor},
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

	t.Skip("thermal chain placement probe complete; turbine, battery, and 32-EU/t state assertions pending")
}
