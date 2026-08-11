package integration

import (
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
)

var craftWorkbenchPos = [3]int32{200, 50, 200}

// TC3: Craft — valid recipe (wood planks 2x2 → crafting table).
// TC4: Craft — invalid recipe (random items → failure).
func TestCrafting_RequestResponse(t *testing.T) {
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()

	playerID := uint64(42)

	// First place the workbench block so there's an entity at that position.
	t.Run("PlaceWorkbench", func(t *testing.T) {
		fbData := testutil.BuildSetBlockAction(playerID, 200, 50, 200, 0, 200)
		if err := c.SendCtrl(testutil.MsgSetBlockAction, fbData); err != nil {
			t.Fatalf("send SetBlockAction: %v", err)
		}
		if _, err := c.ExpectMsgType(testutil.MsgBlockAck, 5*time.Second); err != nil {
			t.Fatalf("expect BlockAck for workbench: %v", err)
		}
	})

	t.Run("ValidRecipe", func(t *testing.T) {
		// Crafting table recipe: 2x2 wood planks → crafting table
		// Grid layout (3x3):
		//   [5,0] [5,0] [0,0]
		//   [5,0] [5,0] [0,0]
		//   [0,0] [0,0] [0,0]
		// where 5 = wood planks (item_id=5)
		grid := [][3]uint16{
			{13, 1, 0}, {13, 1, 0}, {0, 0, 0},
			{13, 1, 0}, {13, 1, 0}, {0, 0, 0},
			{0, 0, 0}, {0, 0, 0}, {0, 0, 0},
		}
		fbData := testutil.BuildCraftRequest(playerID,
			craftWorkbenchPos[0], craftWorkbenchPos[1], craftWorkbenchPos[2], grid)

		if err := c.SendCtrl(testutil.MsgCraftRequest, fbData); err != nil {
			t.Fatalf("send CraftRequest: %v", err)
		}

		data, err := c.ExpectMsgType(testutil.MsgCraftResponse, 5*time.Second)
		if err != nil {
			t.Fatalf("expect CraftResponse: %v", err)
		}

		resp := testutil.AssertCraftResponse(t, data, true)
		result := resp.Result(nil)
		if result == nil {
			t.Fatal("CraftResponse: result is nil")
		}
		t.Logf("Craft result: item_id=%d count=%d meta=%d",
			result.ItemId(), result.Count(), result.Meta())
	})

	t.Run("InvalidRecipe", func(t *testing.T) {
		// Random items that don't form any recipe
		grid := [][3]uint16{
			{1, 1, 0}, {2, 1, 0}, {3, 1, 0},
			{4, 1, 0}, {5, 1, 0}, {6, 1, 0},
			{7, 1, 0}, {8, 1, 0}, {9, 1, 0},
		}
		fbData := testutil.BuildCraftRequest(playerID,
			craftWorkbenchPos[0], craftWorkbenchPos[1], craftWorkbenchPos[2], grid)

		if err := c.SendCtrl(testutil.MsgCraftRequest, fbData); err != nil {
			t.Fatalf("send CraftRequest: %v", err)
		}

		data, err := c.ExpectMsgType(testutil.MsgCraftResponse, 5*time.Second)
		if err != nil {
			t.Fatalf("expect CraftResponse: %v", err)
		}

		testutil.AssertCraftResponse(t, data, false)
	})
}
