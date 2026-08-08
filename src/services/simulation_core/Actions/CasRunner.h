#pragma once
#include "Actions/ActionContext.h"
#include <functional>

namespace simcore {

// Shared setBlockCAS machinery for block actions: publishes the optimistic
// ACCEPTED ack, issues the CAS, and on completion marshals the result back
// to the main thread (postToMain) where the CONFLICT ack or onCommitted run.
// Only copies are captured inside the async callback, so it is safe when the
// CAS result fires on a different thread.
void runBlockCas(const ActionContext& ctx, int32_t eff_x, int32_t eff_y,
                 int32_t eff_z, uint16_t eff_expected, uint16_t final_block_id,
                 uint8_t final_meta, std::function<void()> onCommitted);

} // namespace simcore
