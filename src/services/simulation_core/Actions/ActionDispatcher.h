#pragma once
#include "Actions/handlers/BreakBlockHandler.h"
#include "Actions/handlers/ChestInteractHandler.h"
#include "Actions/handlers/MachineInteractHandler.h"
#include "Actions/handlers/PlaceBlockHandler.h"
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
