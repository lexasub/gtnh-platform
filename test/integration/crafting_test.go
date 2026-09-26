package integration

import (
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
)

var craftWorkbenchPos = [3]int32{200, 50, 200}

// Packed ids from src/content/data/registry/items.csv, resolved with
// ItemId::pack (src/engine/registry/ItemId.h:85-128). The old literals 5/13/14
// predate the packed-id scheme and match no registry entry.
const (
	oakPlanksID   uint16 = 16384 // "0:10:00:0"
	craftTableID  uint16 = 22529 // "0:10:11:1"
	stickID       uint16 = 30720 // "0:11110:0"
	cobbleItemID  uint16 = 2     // "0:0:2"
	ironIngotID   uint16 = 24577 // "0:110:1"
	ironOreItemID uint16 = 32768 // "10:0"
	coalItemID    uint16 = 30722 // "0:11110:2"
)

// benchSession is a snapshot-driven view of one open workbench.
//
// Gateway forwards server pushes without correlation, so the test never
// consumes a frame speculatively: every read goes through wait() with a
// predicate, and the most recent non-matching snapshot is kept in `last` so
// repeated waits still make progress instead of racing past the frame they
// wanted.
type benchSession struct {
	t        *testing.T
	c        *testutil.GatewayClient
	playerID uint64
	last     []byte
	// benchPos is the crafting table's ACTUAL block position. The workbench
	// grid is keyed by block position (WorkbenchStateManager), so a CraftRequest
	// aimed at a guessed y would read a different, always-empty grid.
	benchPos [3]int32
}

func (b *benchSession) update() *Protocol.InventoryUpdate {
	return Protocol.GetRootAsInventoryUpdate(b.last, 0)
}

// wait blocks until pred is satisfied by an InventoryUpdate for this player,
// buffering the last seen snapshot. Unrelated pushes (block acks, grid
// updates, snapshots for other players) are skipped, and the newest
// non-matching snapshot is retained for the next call.
func (b *benchSession) wait(what string, pred func(*Protocol.InventoryUpdate) bool) {
	b.t.Helper()
	// The retained snapshot may already satisfy this predicate — a previous
	// step observed it before its own predicate matched. Re-check it instead of
	// blocking on a frame the server has no reason to send again.
	if len(b.last) != 0 {
		if update := Protocol.GetRootAsInventoryUpdate(b.last, 0); update != nil &&
			update.PlayerId() == b.playerID && pred(update) {
			return
		}
	}
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		msgType, data, err := b.c.ReadCtrl(time.Until(deadline))
		if err != nil {
			b.t.Fatalf("%s: read snapshot: %v", what, err)
		}
		// A zero-length FlatBuffer has no root table; GetRootAs* would panic.
		if msgType != testutil.MsgInventoryUpdate || len(data) == 0 {
			continue
		}
		update := Protocol.GetRootAsInventoryUpdate(data, 0)
		if update.PlayerId() != b.playerID {
			continue
		}
		b.last = data
		if pred(update) {
			return
		}
	}
	b.t.Fatalf("%s: no matching InventoryUpdate for player %d", what, b.playerID)
}

// grantItem is the production creative-menu grant path
// (src/game/actions/PlayerActionDispatcher.cpp:20-29, routed from Gateway's
// player.actions topic). It is how items enter an inventory in this headless
// harness, exactly as the real client does it.
func (b *benchSession) grant(itemID uint16, count byte) {
	b.t.Helper()
	if err := b.c.SendCtrl(testutil.MsgPlayerAction, testutil.BuildPlayerAction(
		b.playerID, Protocol.PlayerActionTypeITEM_ACTION, 0, 0, 0, itemID, count)); err != nil {
		b.t.Fatalf("grant item %d: %v", itemID, err)
	}
	b.wait("grant item", func(u *Protocol.InventoryUpdate) bool {
		return testutil.InventoryItemCount(b.last, itemID) >= int(count)
	})
}

// openWorkbench places a crafting table and opens its window. The window
// registers the per-player container session that container_id=1 clicks
// require (src/apps/simcore/Network/WorkbenchOpenHandler.cpp:27-35).
func (b *benchSession) open(pos [3]int32, reqID uint32) {
	b.t.Helper()
	if err := b.c.RequestChunk(b.playerID, pos[0]>>5, pos[1]>>5, pos[2]>>5); err != nil {
		b.t.Fatalf("request chunk: %v", err)
	}
	b.c.WaitForChunkGeneration(4 * time.Second)

	cs, err := testutil.DialChunkStore("127.0.0.1", 5001, 5*time.Second)
	if err != nil {
		b.t.Fatalf("dial ChunkStore: %v", err)
	}
	defer cs.Close()
	y := airCellAbove(b.t, cs, pos[0], pos[1], pos[2])

	if err := b.c.PlaceBlockAndWait(cs, b.playerID, pos[0], y, pos[2], craftTableID,
		reqID, 8*time.Second); err != nil {
		b.t.Fatalf("place crafting table: %v", err)
	}
	b.benchPos = [3]int32{pos[0], y, pos[2]}
	if err := b.c.SendCtrl(testutil.MsgWorkbenchOpenReq,
		testutil.BuildContainerOpenReq(b.playerID, pos[0], y, pos[2])); err != nil {
		b.t.Fatalf("open workbench: %v", err)
	}
	// The open publishes the authoritative 9-cell empty grid.
	b.wait("open workbench", func(u *Protocol.InventoryUpdate) bool {
		return u.ContainerSlotsLength() == 9 && u.ContainerId() == 1
	})
}

// fillGrid puts one item into each of the workbench grid cells, the way a
// player does it: pick the stack up once, then right-click each target cell so
// exactly one item lands per cell (InventoryClick.h:132-138 — a right-click
// into an empty cell places one and leaves the rest on the cursor).
//
// Crafting is server-authoritative: CraftRequestHandler::handle IGNORES the
// client-supplied CraftRequest slots and reads the grid from
// WorkbenchStateManager (src/game/crafting/CraftRequestHandler.cpp:43-49), and
// InventoryActionHandler persists the grid back to it after every
// container_id=1 click (src/game/storage/InventoryActionHandler.cpp:116-129).
// Populating the grid through real clicks is therefore the only supported way.
func (b *benchSession) fillGrid(itemID uint16, cells ...uint16) {
	b.t.Helper()

	// Find the slot that actually holds the item; the grant picks the first
	// free slot, which is not necessarily 0.
	b.wait("locate source slot", func(u *Protocol.InventoryUpdate) bool {
		return testutil.FirstSlotWithItem(b.last, itemID) >= 0
	})
	srcSlot := uint16(testutil.FirstSlotWithItem(b.last, itemID))

	// Left-click the source slot: the whole stack moves to the server-owned
	// cursor (InventoryClick.h:148-153).
	if err := b.c.SendCtrl(testutil.MsgInventoryAction,
		testutil.BuildInventoryActionWithOptions(b.playerID, 0, 0, 0, 0, srcSlot, 0)); err != nil {
		b.t.Fatalf("pick up item %d from slot %d: %v", itemID, srcSlot, err)
	}
	b.wait("pick up onto cursor", func(u *Protocol.InventoryUpdate) bool {
		var cursor Protocol.ItemStack
		return u.Cursor(&cursor) != nil && cursor.ItemId() == itemID && cursor.Count() > 0
	})

	for _, cell := range cells {
		if err := b.c.SendCtrl(testutil.MsgInventoryAction,
			testutil.BuildInventoryActionWithOptions(b.playerID,
				0 /*kActionClick*/, 1 /*kButtonRight*/, 0, 1, /*container_id=workbench*/
				cell, 0)); err != nil {
			b.t.Fatalf("place item %d into grid cell %d: %v", itemID, cell, err)
		}
		b.wait("place into grid cell", func(u *Protocol.InventoryUpdate) bool {
			var slot Protocol.InventorySlot
			return int(cell) < u.ContainerSlotsLength() &&
				u.ContainerSlots(&slot, int(cell)) && slot.ItemId() == itemID
		})
	}
}

// craft sends the CraftRequest at the bench's real position and returns the
// response. The embedded grid is ignored by the server (see fillGrid).
func (b *benchSession) craft() *Protocol.CraftResponse {
	b.t.Helper()
	p := b.benchPos
	// The grid is server-authoritative and therefore ignored, but the
	// FlatBuffer still has to be well formed: a 9-cell vector of empty stacks.
	emptyGrid := make([][3]uint16, 9)
	if err := b.c.SendCtrl(testutil.MsgCraftRequest,
		testutil.BuildCraftRequest(b.playerID, p[0], p[1], p[2], emptyGrid)); err != nil {
		b.t.Fatalf("send CraftRequest: %v", err)
	}
	data, err := b.c.ExpectMsgType(testutil.MsgCraftResponse, 5*time.Second)
	if err != nil {
		b.t.Fatalf("expect CraftResponse: %v", err)
	}
	return Protocol.GetRootAsCraftResponse(data, 0)
}

// TestCrafting_RequestResponse covers the server-authoritative craft path: a
// real "2x2 oak_planks → crafting_table" grid succeeds and a grid matching no
// recipe is rejected with a reason.
//
// Each case uses its own bench position because the workbench grid is
// per-block-position state in WorkbenchStateManager and a craft only clears
// the cells it consumed.
func TestCrafting_RequestResponse(t *testing.T) {
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()

	const playerID = uint64(42)

	// TC3: 2x2 oak_planks → 1 crafting_table (crafting_table.yaml:9-20).
	t.Run("ValidRecipe", func(t *testing.T) {
		b := &benchSession{t: t, c: c, playerID: playerID}
		pos := [3]int32{200, 50, 200}
		b.open(pos, 44001)
		b.grant(oakPlanksID, 4)
		// Pattern [oak_planks, oak_planks, ~] twice → cells 0,1,3,4.
		b.fillGrid(oakPlanksID, 0, 1, 3, 4)

		resp := b.craft()
		if resp == nil {
			t.Fatal("CraftResponse: nil root")
		}
		if !resp.Success() {
			t.Fatalf("craft failed: %s", string(resp.Error()))
		}
		result := resp.Result(nil)
		if result == nil {
			t.Fatal("CraftResponse: result is nil")
		}
		if result.ItemId() != craftTableID || result.Count() == 0 {
			t.Fatalf("craft result item=%d count=%d, want item=%d count>=1",
				result.ItemId(), result.Count(), craftTableID)
		}
		t.Logf("Craft result: item_id=%d count=%d meta=%d",
			result.ItemId(), result.Count(), result.Meta())
	})

	// TC4: a grid matching no recipe is rejected.
	t.Run("InvalidRecipe", func(t *testing.T) {
		b := &benchSession{t: t, c: c, playerID: playerID}
		pos := [3]int32{203, 50, 200}
		b.open(pos, 44002)
		// A vertical column of iron_ingot matches no crafting_table recipe.
		b.grant(ironIngotID, 3)
		b.fillGrid(ironIngotID, 0, 3, 6)

		resp := b.craft()
		if resp == nil {
			t.Fatal("CraftResponse: nil root")
		}
		if resp.Success() {
			t.Fatalf("craft unexpectedly succeeded for a non-recipe grid")
		}
		if string(resp.Error()) == "" {
			t.Error("a rejected craft must carry a reason")
		}
		t.Logf("Craft correctly rejected: %s", string(resp.Error()))
	})
}
