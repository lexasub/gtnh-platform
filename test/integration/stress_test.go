package integration

import (
	"fmt"
	"sync"
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
)

func TestStress_ConcurrentBlockPlacement(t *testing.T) {
	const numClients = 5
	errs := make(chan error, numClients)
	var wg sync.WaitGroup

	for i := 0; i < numClients; i++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			c, err := testutil.DialGateway(gw, 5*time.Second)
			if err != nil {
				errs <- err
				return
			}
			defer c.Close()

			x := int32(600 + id)
			// Face 0 is DOWN, so the server's faceAdjacentBlock shifts the target one
			// cell down (ActionContext.cpp:47, ActionContext.h:21). Send the cell the
			// block is FACING, not the cell it goes into, or the CAS lands a block
			// lower than the test intended. y=120 therefore places at y=119.
			// stoneID, carried in hand: an empty-hand frame is rejected by
			// PlaceBlockHandler::canHandle (PlaceBlockHandler.cpp:14-18).
			fbData := testutil.BuildPlaceBlockAction(uint64(100+id), x, 120, 600, 0, stoneID)
			if err := c.SendCtrl(testutil.MsgSetBlockAction, fbData); err != nil {
				errs <- err
				return
			}
			data, err := c.ExpectMsgType(testutil.MsgBlockAck, 10*time.Second)
			if err != nil {
				errs <- err
				return
			}
			ack := Protocol.GetRootAsBlockAck(data, 0)
			if ack.Status() != Protocol.BlockAckStatusACCEPTED {
				// Report the status, not the (nil) read error: sending nil here made a
				// non-ACCEPTED placement look like success to the loop below.
				errs <- fmt.Errorf("block %d: expected ACCEPTED, got %v", id, ack.Status())
			}
		}(i)
	}
	wg.Wait()
	close(errs)

	for err := range errs {
		if err != nil {
			t.Errorf("concurrent block placement error: %v", err)
		}
	}
}

func TestStress_CASRace(t *testing.T) {
	pos := [3]int32{650, 120, 650}

	result := make(chan int, 2)
	for i := 0; i < 2; i++ {
		go func(id int) {
			c, err := testutil.DialGateway(gw, 5*time.Second)
			if err != nil {
				t.Logf("client %d: dial error: %v", id, err)
				result <- 0
				return
			}
			defer c.Close()

			// Face 0 is DOWN, so the server shifts the placement one cell down
			// (ActionContext.cpp:47). stoneID is carried in hand; the old literal 42
			// is not a real item and was rejected by PlaceBlockHandler::canHandle.
			fbData := testutil.BuildPlaceBlockAction(uint64(200+id), pos[0], pos[1], pos[2], 0, stoneID)
			c.SendCtrl(testutil.MsgSetBlockAction, fbData)

			deadline := time.Now().Add(10 * time.Second)
			for time.Now().Before(deadline) {
				data, err := c.ExpectMsgType(testutil.MsgBlockAck, 1*time.Second)
				if err != nil {
					continue
				}
				ack := Protocol.GetRootAsBlockAck(data, 0)
				if ack.Status() == Protocol.BlockAckStatusCONFLICT {
					result <- 2
					return
				}
				if ack.Status() == Protocol.BlockAckStatusACCEPTED {
					result <- 1
					return
				}
			}
			result <- 0
		}(i)
	}

	statuses := []int{<-result, <-result}
	if statuses[0] == statuses[1] && statuses[0] == 1 {
		t.Log("both got ACCEPTED (optimistic CAS)")
	} else if statuses[0] == 1 && statuses[1] == 2 || statuses[0] == 2 && statuses[1] == 1 {
		t.Log("one ACCEPTED, one CONFLICT — CAS race resolved correctly")
	} else {
		t.Errorf("unexpected CAS race results: %v", statuses)
	}
}

func TestStress_RapidFireBlocks(t *testing.T) {
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()

	const n = 10
	for i := 0; i < n; i++ {
		x := int32(700 + i)
		fbData := testutil.BuildPlaceBlockAction(42, x, 120, 700, 0, stoneID)
		if err := c.SendCtrl(testutil.MsgSetBlockAction, fbData); err != nil {
			t.Fatalf("send %d: %v", i, err)
		}
	}

	for i := 0; i < n; i++ {
		data, err := c.ExpectMsgType(testutil.MsgBlockAck, 10*time.Second)
		if err != nil {
			t.Fatalf("expect ack %d: %v", i, err)
		}
		ack := Protocol.GetRootAsBlockAck(data, 0)
		if ack.Status() != Protocol.BlockAckStatusACCEPTED {
			t.Errorf("block %d: expected ACCEPTED, got %v", i, ack.Status())
		}
	}
}
