package main

import (
	"testing"

	flatbuffers "github.com/google/flatbuffers/go"

	"github.com/gtnh-platform/protocol/generated/go/Protocol"
)

// buildQuestProgressUpdate mirrors the SimCore PlayerJoinedHandler request:
// player_id set, optional quests vector (empty = query semantics).
func buildQuestProgressUpdate(b *flatbuffers.Builder, playerID uint64, quests []struct {
	questID  uint32
	status   Protocol.QuestStatus
	progress byte
}) []byte {
	questOffsets := make([]flatbuffers.UOffsetT, 0, len(quests))
	for _, q := range quests {
		Protocol.QuestEntryStart(b)
		Protocol.QuestEntryAddQuestId(b, q.questID)
		Protocol.QuestEntryAddStatus(b, q.status)
		Protocol.QuestEntryAddProgress(b, q.progress)
		questOffsets = append(questOffsets, Protocol.QuestEntryEnd(b))
	}
	Protocol.QuestProgressUpdateStartQuestsVector(b, len(quests))
	for i := len(questOffsets) - 1; i >= 0; i-- {
		b.PrependUOffsetT(questOffsets[i])
	}
	questsVec := b.EndVector(len(quests))
	Protocol.QuestProgressUpdateStart(b)
	Protocol.QuestProgressUpdateAddPlayerId(b, playerID)
	Protocol.QuestProgressUpdateAddQuests(b, questsVec)
	offset := Protocol.QuestProgressUpdateEnd(b)
	b.Finish(offset)
	return b.FinishedBytes()
}

// TestQuestProgressUpdateWireFormat verifies the FlatBuffers quest wire
// contract: empty-vector request (as sent by SimCore) parses with the
// expected player_id, and a response with entries round-trips.
func TestQuestProgressUpdateWireFormat(t *testing.T) {
	b := flatbuffers.NewBuilder(128)

	req := buildQuestProgressUpdate(b, 42, nil)
	if len(req) == 0 {
		t.Fatal("empty request buffer")
	}
	r := Protocol.GetRootAsQuestProgressUpdate(req, 0)
	if r == nil {
		t.Fatal("failed to parse request")
	}
	if r.PlayerId() != 42 {
		t.Fatalf("player_id = %d, want 42", r.PlayerId())
	}
	if r.QuestsLength() != 0 {
		t.Fatalf("request quests len = %d, want 0 (query semantics)", r.QuestsLength())
	}

	b2 := flatbuffers.NewBuilder(256)
	resp := buildQuestProgressUpdate(b2, 42, []struct {
		questID  uint32
		status   Protocol.QuestStatus
		progress byte
	}{
		{questID: 7, status: Protocol.QuestStatusCOMPLETED, progress: 100},
		{questID: 9, status: Protocol.QuestStatusAVAILABLE, progress: 0},
	})
	s := Protocol.GetRootAsQuestProgressUpdate(resp, 0)
	if s == nil {
		t.Fatal("failed to parse response")
	}
	if s.QuestsLength() != 2 {
		t.Fatalf("response quests len = %d, want 2", s.QuestsLength())
	}
	entry := new(Protocol.QuestEntry)
	if !s.Quests(entry, 0) {
		t.Fatal("failed to read quest[0]")
	}
	if entry.QuestId() != 7 || entry.Status() != Protocol.QuestStatusCOMPLETED || entry.Progress() != 100 {
		t.Fatalf("quest[0] = (%d, %d, %d), want (7, COMPLETED, 100)",
			entry.QuestId(), entry.Status(), entry.Progress())
	}
	if !s.Quests(entry, 1) {
		t.Fatal("failed to read quest[1]")
	}
	if entry.QuestId() != 9 || entry.Status() != Protocol.QuestStatusAVAILABLE {
		t.Fatalf("quest[1] = (%d, %d), want (9, AVAILABLE)", entry.QuestId(), entry.Status())
	}
}
