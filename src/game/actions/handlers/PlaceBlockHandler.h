#pragma once
#include <cstdint>

#include <common/GameModePerm.h>

namespace simcore {

class ActionContext;

// The player's game mode, as this server has it, resolved into the shared
// permission matrix.
//
// THE AUTHORITATIVE GATE'S INPUT (gp-t71g). Every other place that decides
// "may this mode place a block" is asking the same question of the same
// matrix; this is the one function that asks it of the SERVER's own record of
// the player's mode, which is the only one that cannot be lied to by a client.
//
// Where the mode comes from: PlayerInventoryStore, which already stored it and
// which SimCoreMessageHandler.cpp:334 populates from the GameModeChange frame.
// The earlier finding that the store "carries gameMode and no action handler
// ever reads it" is exactly what made this handler gateable at all — the
// value was already there, unconsumed.
//
// WHY IT RE-STATES ONE LINE rather than reaching for the store's default
// silently: with no inventory store attached there is no player record to
// consult, and a placement with no store attached charges nothing anyway
// (the decrement is already guarded on the store being non-null). Resolving
// that case to SURVIVAL — the store's own default for an unknown player, and
// a mode CanPlace allows — keeps "no store" and "a player nobody has seen
// yet" from disagreeing, and fails OPEN only in the one configuration where
// no inventory is being written anyway.
inline bool CanPlaceBlocksOnServer(uint8_t stored_mode) {
  return GameModePerm::CanPlace(static_cast<GameMode>(stored_mode));
}

// Why a placement frame that PlaceBlockHandler declined was declined by the
// MODE gate, or nullptr when it was declined for a reason that is not this
// gate's (wrong action type, nothing placeable in hand, a tool in hand, or —
// and this is the case that matters most — a frame that was never a placement
// because an earlier handler in the tuple claimed it).
//
// WHY THIS EXISTS. Refusing by not claiming (see PlaceBlockHandler::canHandle)
// means the frame falls through to SetBlockCASHandler's fallback, and that
// fallback has exactly one hard-coded string: "nothing placeable in hand". For
// a SPECTATOR who IS holding a block, that reason is simply false, and a false
// REJECTED reason is worse than none — it points an operator at the client
// instead of at the mode.
//
// So the gate cannot both (a) decline the frame and (b) stay a pure predicate.
// Rather than giving canHandle an out-parameter, or having the handler claim
// the frame and early-return (which would suppress the facade's ack entirely),
// the decision is exposed as a separate question the facade may ask once
// dispatch has come back unhandled. Both read the same value the same way, so
// they cannot disagree; and only one of them reports a reason, so a frame is
// never described as both placeable and refused.
//
// Right-click placement: place the held block on the face-adjacent cell
// (transform rules apply), consume it from the player inventory on success
// and fire the block-placed hook.
//
// GATED ON GAME MODE (gp-t71g): canHandle() requires
// CanPlaceBlocksOnServer, so a mode the matrix denies is not claimed at all
// and the dispatcher falls through to the facade's REJECTED ack.
//
// WHY THE MODE COMES FROM THE STORE: the store is the server's own record,
// populated from the GameModeChange frame, and NOT from anything on the
// placement action — the action carries no mode, and a client-supplied one
// would make this gate exactly as trustworthy as the client being gated. This
// is the AUTHORITATIVE half: the client's own gate in GameClient.cpp only
// stops the shipped client from asking, while this stops any client and
// survives a modified one, a replayed frame or a future second client.
//
// WHY IT REFUSES BY NOT CLAIMING (rather than claiming and early-returning):
// ActionDispatcher::dispatch reports "handled" as the OR over the handler
// tuple, so a handler that claimed the frame and then did nothing would
// SUPPRESS the facade's REJECTED ack (SetBlockCASHandler.cpp:47-56) — the
// client would keep an optimistic ACCEPTED that never resolves and a block
// that silently never appears. Declining hands the frame back to the facade,
// which answers REJECTED with RefusalReasonForMode() when the mode was the
// cause and the generic reason otherwise.
//
// WHY THAT MAKES THE REFUSAL STRUCTURALLY BEFORE THE CHARGE: the inventory
// decrement lives inside the runBlockCas commit callback in handle(), and a
// frame this function declines never reaches it. There is no ordering to get
// wrong and no path on which a refused placement has charged anything,
// because there is no path on which it reaches the charge. A gate placed
// after the decrement would pass a "no block appeared in the world" test
// while still destroying the player's item, which is the failure mode the
// tests below are written to catch.
class PlaceBlockHandler {
public:
  bool canHandle(const ActionContext& ctx) const;
  void handle(const ActionContext& ctx) const;

  static const char* RefusalReasonForMode(const ActionContext& ctx);
};

} // namespace simcore
