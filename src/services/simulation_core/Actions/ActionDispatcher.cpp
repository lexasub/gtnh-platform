#include "Actions/ActionDispatcher.h"
#include "Actions/ActionContext.h"
#include <tuple>

namespace simcore {

bool ActionDispatcher::dispatch(ActionContext& ctx) const {
  bool handled = false;
  std::apply(
      [&](const auto&... h) {
        ((handled || (h.canHandle(ctx) ? (h.handle(ctx), true) : false)) || ...);
      },
      handlers_);
  return handled;
}

} // namespace simcore
