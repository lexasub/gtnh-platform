package integration

import (
	"testing"
	"time"

	"github.com/gtnh-platform/integration-tests/testutil"
	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
)

// TestQuestBookOpen_InventoryCompletion exercises the production client path:
// client Ctrl frame 33 -> Gateway topic quest.book.open -> SimCore inventory
// check -> QuestManager completion -> Gateway notifications.
func TestQuestBookOpen_InventoryCompletion(t *testing.T) {
	// TestMain starts one process-scoped cluster and every Ctrl connection is
	// assigned player 1, so this tracer can prove its cause exactly once:
	// quest state recorded on the first run suppresses the second run's
	// completion notification and it would fail later with an unrelated read
	// timeout. Claiming the run turns that into an explicit contract. The key
	// is t.Name() so a rename cannot silently mint a second claim slot.
	if err := testutil.ClaimSingleRun(t.Name()); err != nil {
		t.Fatalf("repeat run: %v", err)
	}

	c, err := testutil.DialGateway(gw, 5*time.Second)
	if err != nil {
		t.Fatalf("dial Gateway: %v", err)
	}
	defer c.Close()

	// A creative spawn uses the production client path: switch the authoritative
	// server game mode first, wait for the echo, then use the same ITEM_ACTION
	// emitted by the game client's creative menu.
	// TestMain's cluster is process-scoped; player 1 is deliberately the fixed
	// identity used by the production client path in this focused tracer.
	const playerID = uint64(1)
	if err := c.SendCtrl(testutil.MsgGameModeChange,
		testutil.BuildGameModeChange(playerID, Protocol.GameModeCREATIVE)); err != nil {
		t.Fatalf("switch to creative: %v", err)
	}
	modeData, err := c.ExpectMsgType(testutil.MsgGameModeChange, 5*time.Second)
	if err != nil {
		t.Fatalf("wait for authoritative creative mode: %v", err)
	}
	mode := Protocol.GetRootAsGameModeChange(modeData, 0)
	if mode.PlayerId() != playerID || mode.NewMode() != Protocol.GameModeCREATIVE {
		t.Fatalf("creative echo player=%d mode=%v", mode.PlayerId(), mode.NewMode())
	}
	if err := c.SendCtrl(testutil.MsgPlayerAction, testutil.BuildPlayerAction(
		playerID, Protocol.PlayerActionTypeITEM_ACTION, 0, 0, 0, 22530, 8)); err != nil {
		t.Fatalf("send creative oak grant: %v", err)
	}
	// Gateway forwards queued player.inventory.update pushes without
	// correlation, so wait for the snapshot that actually proves this grant
	// (player 1 holding at least 8 oak logs) instead of the first frame.
	data, err := c.WaitForInventoryItem(playerID, 22530, 8, 5*time.Second)
	if err != nil {
		t.Fatalf("wait for authoritative oak inventory update: %v", err)
	}
	update := Protocol.GetRootAsInventoryUpdate(data, 0)
	count := 0
	var slot Protocol.InventorySlot
	for i := 0; i < update.SlotsLength(); i++ {
		if update.Slots(&slot, i) && slot.ItemId() == 22530 {
			count += int(slot.Count())
		}
	}
	if update.PlayerId() != playerID || count < 8 {
		t.Fatalf("oak inventory = player %d count %d, want player %d count >= 8", update.PlayerId(), count, playerID)
	}

	if err := c.SendCtrl(testutil.MsgQuestBookOpen, testutil.BuildQuestBookOpen(playerID)); err != nil {
		t.Fatalf("send QuestBookOpen: %v", err)
	}

	completed, progress, err := c.ExpectQuestCompletion(playerID, 1, 5*time.Second)
	if err != nil {
		t.Fatalf("QuestBookOpen did not complete quest 1 through client→Gateway→SimCore: %v", err)
	}
	notification := Protocol.GetRootAsQuestCompletedNotification(completed, 0)
	if notification.PlayerId() != playerID || notification.QuestId() != 1 {
		t.Fatalf("completion notification player=%d quest=%d, want player=%d quest=1", notification.PlayerId(), notification.QuestId(), playerID)
	}
	progressUpdate := Protocol.GetRootAsQuestProgressUpdate(progress, 0)
	var entry Protocol.QuestEntry
	if !progressUpdate.Quests(&entry, 0) || entry.QuestId() != 1 || entry.Status() != Protocol.QuestStatusCOMPLETED || entry.Progress() != 100 {
		t.Fatal("completion progress update did not contain quest 1 COMPLETED/100")
	}
}
