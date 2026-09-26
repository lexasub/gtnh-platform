#include <game/actions/ActionDispatcher.h>
#include <game/actions/ActionContext.h>
#include <tuple>

namespace simcore {

// Routes to the first handler whose canHandle() is true and reports whether one
// claimed the action. The fold's value MUST be assigned back to `handled`: the
// chain itself is what short-circuits (only the claiming handler runs), and its
// result is the only thing that records that a handler ran. Dropping it made
// dispatch() unconditionally return false, which turned the facade's
// "nothing placeable in hand" rejection into a spurious ack on every handled
// action (see test_action_dispatch.cpp, gp-qij1).
bool ActionDispatcher::dispatch(ActionContext& ctx) const {
  bool handled = false;
  std::apply(
      [&](const auto&... h) {
        handled = ((handled || (h.canHandle(ctx) ? (h.handle(ctx), true) : false)) ||
                   ...);
      },
      handlers_);
  return handled;
}

} // namespace simcore
