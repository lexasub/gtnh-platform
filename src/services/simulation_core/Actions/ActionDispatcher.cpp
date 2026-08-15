#include "Actions/ActionDispatcher.h"
#include "Actions/ActionContext.h"
#include <tuple>

namespace simcore {

bool ActionDispatcher::dispatch(ActionContext& ctx) const {
  bool handled = false;
  std::apply(
      [&](const auto&... h) {
        // Stop at the first matching handler: `!handled` guards each step so
        // only one handler runs, and `handled` is written (a prior version only
        // read it, so dispatch always returned false).
        (((!handled && h.canHandle(ctx)) &&
          (h.handle(ctx), handled = true, true)) ||
         ...);
      },
      handlers_);
  return handled;
}

} // namespace simcore
