package testutil

import (
	"testing"

	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
)

func TestDecodeMultiblockEventRejectsEmpty(t *testing.T) {
	if _, _, _, err := DecodeMultiblockEvent(nil); err == nil {
		t.Fatal("expected empty event error")
	}
}

func TestBuildSetBlockActionWithOptionsCarriesRequestID(t *testing.T) {
	data := BuildSetBlockActionWithOptions(1, 2, 3, 4, 0, 1001, SetBlockActionOptions{RequestID: 77, Face: 5, HeldItem: 1001})
	action := Protocol.GetRootAsSetBlockAction(data, 0)
	if action.RequestId() != 77 || action.Face() != 5 || action.HeldItem() != 1001 {
		t.Fatalf("options not encoded: request=%d face=%d held=%d", action.RequestId(), action.Face(), action.HeldItem())
	}
}
