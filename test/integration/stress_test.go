package integration

import (
	"fmt"
	"sync"
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
)

// SINGLE-PLAYER GATEWAY CONTRACT
//
// These two tests open more than one client connection, and the Gateway does not
// support that yet - by design, not by accident:
//
//   gateway.h:112  std::unique_ptr<IoUringConnection> client_ctrl_;   ONE socket, not a map
//   gateway.cpp:90-94  a new ctrl connection MOVES the old one out and destroys it
//   gateway.cpp:112    CreatePlayerJoined(fbb, 1) - every client is announced as player 1
//
// So client N+1 evicts client N, and player.actions.ack has no routing key: the ack for
// a CAS outcome is written to whichever single socket is currently attached, and the
// payload's player_id is never consulted. These tests therefore cannot observe a
// multi-client exchange today.
//
// What IS verified, and what these tests are really for: the SERVER-side CAS is correct.
// Reading the log of a failing run shows
//     Block CAS OK at (650,119,650) final_id=1
//     Block CAS CONFLICT at (650,119,650) actual_id=1, from_id=0, to_id=1
// i.e. one writer wins and the other is told it conflicted, exactly as designed. The
// failures are the harness observing a capability the server does not claim to have.
//
// The assertions below are deliberately NOT relaxed to accept the observed values. Doing
// so would assert that a silently-dropped ack is acceptable behaviour. Multi-client
// routing is filed separately; until it lands these tests stay red and say why.

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
