#pragma once
#include <game/actions/handlers/BreakBlockHandler.h>
#include <game/actions/handlers/ChestInteractHandler.h>
#include <game/actions/handlers/MachineInteractHandler.h>
#include <game/actions/handlers/PlaceBlockHandler.h>
#include <tuple>

namespace simcore {

class ActionContext;

// Routes a SetBlockAction to the first handler whose canHandle() matches.
// Priority = tuple declaration order: machine → chest → break → place.
// A false return means no handler claimed the action; the facade decides the
// fallback (e.g. reject "nothing placeable in hand").
class ActionDispatcher {
public:
  bool dispatch(ActionContext& ctx) const;

private:
  std::tuple<MachineInteractHandler, ChestInteractHandler, BreakBlockHandler,
             PlaceBlockHandler>
      handlers_;
};

} // namespace simcore
