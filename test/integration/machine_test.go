package integration

import (
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
)

var (
	// Two adjacent cells: the furnace at x, the heat generator at genPos.x.
	furnacePos = [3]int32{201, 50, 200}
	genPos     = [3]int32{202, 50, 200}
)

// invSnapshot holds the InventoryUpdate pushes seen so far. Gateway forwards
// them without correlation and the response wait below discards anything that
// is not the type it wants, so a snapshot that proves the assertion can arrive
// DURING a SetMachineSlotResp wait. Buffering them keeps that evidence.
type invSnapshot struct {
	t        *testing.T
	c        *testutil.GatewayClient
	playerID uint64
	seen     [][]byte
}

func (s *invSnapshot) absorb(msgType uint8, data []byte) {
	if msgType == testutil.MsgInventoryUpdate && len(data) != 0 {
		s.seen = append(s.seen, append([]byte(nil), data...))
	}
}

func (s *invSnapshot) count(itemID uint16) int {
	total := 0
	for _, data := range s.seen {
		if u := Protocol.GetRootAsInventoryUpdate(data, 0); u != nil && u.PlayerId() == s.playerID {
			total += testutil.InventoryItemCount(data, itemID)
		}
	}
	return total
}

// sendMachineSlot sends a SetMachineSlotReq and waits for its response, keeping
// any inventory snapshots that arrive in between. Returns the response.
func (s *invSnapshot) sendMachineSlot(what string, x, y, z int32, slot uint16,
	item uint16, count uint8, playerSlot uint8) *Protocol.SetMachineSlotResp {
	s.t.Helper()
	if err := s.c.SendCtrl(testutil.MsgSetMachineSlot,
		testutil.BuildSetMachineSlotReq(s.playerID, x, y, z, slot, item, count, 0, playerSlot)); err != nil {
		s.t.Fatalf("send SetMachineSlotReq (%s): %v", what, err)
	}
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		msgType, data, err := s.c.ReadCtrl(time.Until(deadline))
		if err != nil {
			s.t.Fatalf("%s response: %v", what, err)
		}
		if msgType == testutil.MsgSetMachineSlotResp {
			return Protocol.GetRootAsSetMachineSlotResp(data, 0)
		}
		s.absorb(msgType, data)
	}
	s.t.Fatalf("%s: no SetMachineSlotResp", what)
	return nil
}

// awaitItem drains until the buffered + new snapshots prove the player holds
// at least min of itemID.
func (s *invSnapshot) awaitItem(itemID uint16, min int, timeout time.Duration) {
	s.t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if s.count(itemID) >= min {
			return
		}
		msgType, data, err := s.c.ReadCtrl(time.Until(deadline))
		if err != nil {
			break
		}
		s.absorb(msgType, data)
	}
	s.t.Fatalf("player %d never held %d of item %d (best snapshot total %d)",
		s.playerID, min, itemID, s.count(itemID))
}

// TestCrafting_ValidInvalid covers the "stick" recipe
// (crafting_table.yaml:48-59): two oak_planks stacked vertically produce four
// sticks. It shares the server-authoritative workbench machinery with
// TestCrafting_RequestResponse but exercises a different recipe, and its
// InvalidRecipe case proves a grid that merely has items in it is still
// rejected when it matches nothing.
func TestCrafting_ValidInvalid(t *testing.T) {
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()

	const playerID = uint64(43)

	t.Run("ValidRecipe", func(t *testing.T) {
		b := &benchSession{t: t, c: c, playerID: playerID}
		pos := [3]int32{200, 50, 200}
		b.open(pos, 45001)
		b.grant(oakPlanksID, 2)
		// Pattern [oak_planks, ~, ~] twice → cells 0 and 3.
		b.fillGrid(oakPlanksID, 0, 3)

		resp := b.craft()
		if resp == nil {
			t.Fatal("CraftResponse: nil root")
		}
		if !resp.Success() {
			t.Fatalf("stick craft failed: %s", string(resp.Error()))
		}
		result := resp.Result(nil)
		if result == nil {
			t.Fatal("CraftResponse result is nil")
		}
		// The recipe yields 4 sticks, not one.
		if result.ItemId() != stickID || result.Count() < 4 {
			t.Fatalf("craft result item=%d count=%d, want item=%d count>=4",
				result.ItemId(), result.Count(), stickID)
		}
		t.Logf("Craft result: item_id=%d count=%d meta=%d",
			result.ItemId(), result.Count(), result.Meta())
	})

	t.Run("InvalidRecipe", func(t *testing.T) {
		b := &benchSession{t: t, c: c, playerID: playerID}
		pos := [3]int32{203, 50, 200}
		b.open(pos, 45002)
		// A single item matches no recipe.
		b.grant(ironIngotID, 1)
		b.fillGrid(ironIngotID, 0)

		resp := b.craft()
		if resp == nil {
			t.Fatal("CraftResponse: nil root")
		}
		if resp.Success() {
			t.Fatal("a one-item grid must not match any recipe")
		}
		if string(resp.Error()) == "" {
			t.Error("a rejected craft must carry a reason")
		}
		t.Logf("Craft correctly rejected: %s", string(resp.Error()))
	})
}

// TC8: SetMachineSlotReq — move an item into a machine slot.
// TC9: SetMachineSlotReq — extract the item back out.
func TestMachine_SlotTransfer(t *testing.T) {
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()

	const playerID = uint64(44)

	// heat_furnace is "1110:000:0" packed (machines.yaml:35-45). The old
	// literal 36 predates the packed-id scheme and names no machine.
	const heatFurnaceID uint16 = 0xE000

	if err := c.RequestChunk(playerID, furnacePos[0]>>5, furnacePos[1]>>5, furnacePos[2]>>5); err != nil {
		t.Fatalf("request chunk: %v", err)
	}
	c.WaitForChunkGeneration(4 * time.Second)

	cs, err := testutil.DialChunkStore("127.0.0.1", 5001, 5*time.Second)
	if err != nil {
		t.Fatalf("dial ChunkStore: %v", err)
	}
	defer cs.Close()
	y := airCellAbove(t, cs, furnacePos[0], furnacePos[1], furnacePos[2])

	if err := c.PlaceBlockAndWait(cs, playerID, furnacePos[0], y, furnacePos[2],
		heatFurnaceID, 45100, 8*time.Second); err != nil {
		t.Fatalf("place heat furnace: %v", err)
	}
	// Opening the machine window rehydrates the ECS container that
	// SetMachineSlotReq mutates; the real client does this first.
	if err := c.SendCtrl(testutil.MsgMachineOpenReq,
		testutil.BuildContainerOpenReq(playerID, furnacePos[0], y, furnacePos[2])); err != nil {
		t.Fatalf("open furnace: %v", err)
	}

	snap := &invSnapshot{t: t, c: c, playerID: playerID}

	t.Run("PlaceItemInMachineSlot", func(t *testing.T) {
		// player_slot=255 means "from cursor, not from player inventory"
		// (energy_chain_test.go documents the same convention).
		resp := snap.sendMachineSlot("insert", furnacePos[0], y, furnacePos[2],
			0 /*input slot*/, cobbleItemID, 1, 255)
		if resp == nil || !resp.Success() {
			t.Fatalf("server rejected the machine-slot insert")
		}
	})

	t.Run("ExtractItemFromMachineSlot", func(t *testing.T) {
		// The input slot no longer holds the cobblestone: the running furnace
		// consumed it as soon as MachineSystem matched base:smelting_cobblestone
		// (MachineSystem.cpp:173-181, furnace.yaml:25-32). So the extract acts on
		// an already-empty input slot and correctly yields nothing.
		//
		// To prove the extract path itself, refill the slot with an item no
		// furnace recipe consumes, so MachineSystem cannot race the click.
		resp := snap.sendMachineSlot("refill", furnacePos[0], y, furnacePos[2],
			0 /*input slot*/, stickID, 3, 255)
		if resp == nil || !resp.Success() {
			t.Fatalf("server rejected the machine-slot refill")
		}

		// item_id=0 + count=0 clears the slot; player_slot=5 sends the
		// extracted stack to player slot 5 (MachineSlotHandler.cpp:92-99).
		resp = snap.sendMachineSlot("extract", furnacePos[0], y, furnacePos[2],
			0, 0, 0, 5)
		if resp == nil || !resp.Success() {
			t.Fatalf("server rejected the machine-slot extract")
		}
		// The extracted stack must reach the player grid.
		snap.awaitItem(stickID, 3, 5*time.Second)
	})
}

// TestMachine_GeneratorFurnaceChain proves the full heat chain: a heat
// generator burns coal, HeatTransferSystem passes the HEAT to an adjacent heat
// furnace, and MachineSystem smelts iron_ore into an iron_ingot.
//
// The old version of this test asserted only that the furnace BLOCK still
// existed, which is a tautology — a furnace that never smelted anything
// passes it. It now waits for the smelted output in the machine's
// BlockEntityUpdate stream.
//
// Flow:
//  1. Place heat_generator ("1110:000:2" → 0xE002) adjacent to
//  2. Place heat_furnace ("1110:000:0" → 0xE000)
//  3. Open both machine windows (the ECS container must be rehydrated)
//  4. Put coal ("0:11110:2") in the generator's fuel slot
//  5. Put iron_ore ("10:0") in the furnace's input slot
//  6. Wait for the furnace to report an iron_ingot output
func TestMachine_GeneratorFurnaceChain(t *testing.T) {
	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Skipf("Gateway not reachable: %v", err)
	}
	defer c.Close()

	const playerID = uint64(45)
	// Packed machine ids from src/content/data/registry/machines.yaml. The old
	// literals 36/46/44/3 predate the packed-id scheme and name nothing.
	const (
		heatFurnaceID   uint16 = 0xE000 // "1110:000:0"
		heatGeneratorID uint16 = 0xE002 // "1110:000:2"
	)

	if err := c.RequestChunk(playerID, furnacePos[0]>>5, furnacePos[1]>>5, furnacePos[2]>>5); err != nil {
		t.Fatalf("request chunk: %v", err)
	}
	c.WaitForChunkGeneration(4 * time.Second)

	cs, err := testutil.DialChunkStore("127.0.0.1", 5001, 5*time.Second)
	if err != nil {
		t.Fatalf("dial ChunkStore: %v", err)
	}
	defer cs.Close()
	y := airCellAbove(t, cs, furnacePos[0], furnacePos[1], furnacePos[2])
	// The generator must be ADJACENT for HeatTransferSystem to link the two.
	genCell := [3]int32{genPos[0], y, genPos[2]}

	for i, m := range []struct {
		x, z int32
		id   uint16
	}{
		{furnacePos[0], furnacePos[2], heatFurnaceID},
		{genCell[0], genCell[2], heatGeneratorID},
	} {
		if err := c.PlaceBlockAndWait(cs, playerID, m.x, y, m.z, m.id,
			uint32(45200+i), 8*time.Second); err != nil {
			t.Fatalf("place machine 0x%04X: %v", m.id, err)
		}
	}

	// Opening each window rehydrates the live ECS container that
	// SetMachineSlotReq mutates.
	for _, m := range []struct {
		x, z int32
		id   uint16
	}{{furnacePos[0], furnacePos[2], heatFurnaceID}, {genCell[0], genCell[2], heatGeneratorID}} {
		if err := c.SendCtrl(testutil.MsgMachineOpenReq,
			testutil.BuildContainerOpenReq(playerID, m.x, y, m.z)); err != nil {
			t.Fatalf("open machine 0x%04X: %v", m.id, err)
		}
		if _, err := c.ExpectMsgType(testutil.MsgBlockEntityUpdate, 5*time.Second); err != nil {
			t.Fatalf("open update for 0x%04X: %v", m.id, err)
		}
	}

	insert := func(what string, x, z int32, slot uint16, item uint16, count uint8) {
		t.Helper()
		if err := c.SendCtrl(testutil.MsgSetMachineSlot,
			testutil.BuildSetMachineSlotReq(playerID, x, y, z, slot, item, count, 0, 255)); err != nil {
			t.Fatalf("insert %s: %v", what, err)
		}
		data, err := c.ExpectMsgType(testutil.MsgSetMachineSlotResp, 5*time.Second)
		if err != nil {
			t.Fatalf("insert %s response: %v", what, err)
		}
		if resp := Protocol.GetRootAsSetMachineSlotResp(data, 0); resp == nil || !resp.Success() {
			t.Fatalf("server rejected inserting %s", what)
		}
	}
	insert("coal into generator", genCell[0], genCell[2], 0, coalItemID, 8)
	insert("iron ore into furnace", furnacePos[0], furnacePos[2], 0, ironOreItemID, 1)

	// The furnace can only smelt while it is receiving HEAT from the
	// generator, so poll the machine state stream for the real output rather
	// than sleeping a fixed interval.
	deadline := time.Now().Add(40 * time.Second)
	smelted := false
	for time.Now().Before(deadline) && !smelted {
		msgType, data, err := c.ReadCtrl(500 * time.Millisecond)
		if err != nil || msgType != testutil.MsgBlockEntityUpdate || len(data) == 0 {
			continue
		}
		update := Protocol.GetRootAsBlockEntityUpdate(data, 0)
		var pos Protocol.Vec3i
		if update == nil || update.Pos(&pos) == nil {
			continue
		}
		if pos.X() != furnacePos[0] || pos.Y() != y || pos.Z() != furnacePos[2] {
			continue
		}
		var item Protocol.ItemStack
		for i := 0; i < update.OutputItemsLength(); i++ {
			if update.OutputItems(&item, i) && item.ItemId() == ironIngotID && item.Count() > 0 {
				smelted = true
				t.Logf("furnace smelted %d iron_ingot at progress=%.2f",
					item.Count(), update.Progress())
			}
		}
	}
	if !smelted {
		t.Fatal("heat furnace never produced an iron_ingot: the generator→furnace heat chain did not complete")
	}
}
