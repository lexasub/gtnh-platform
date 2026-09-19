#include "RecipeCompletedHandler.h"
#include <engine/sim/SimulationEngine.h>
#include "recipe_generated.h"
#include <engine/sim/components/MachineComponent.h>
#include <engine/sim/components/InventoryContainer.h>
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

namespace simcore {

RecipeCompletedHandler::RecipeCompletedHandler(std::shared_ptr<SimulationEngine> engine)
    : engine_(std::move(engine))
{}

void RecipeCompletedHandler::handle(const std::vector<uint8_t>& data) {
    flatbuffers::Verifier v(data.data(), data.size());
    if (!v.VerifyBuffer<Protocol::RecipeCompleted>(nullptr)) {
        spdlog::warn("[SimCore] invalid RecipeCompleted buffer");
        return;
    }
    auto* completed = flatbuffers::GetRoot<Protocol::RecipeCompleted>(data.data());
    if (!completed || !completed->pos() || !completed->result_slots()) return;

    auto* pos = completed->pos();
    int32_t x = pos->x(), y = pos->y(), z = pos->z();

    auto view = engine_->reg().view<MachineComponent, InventoryContainer>();
    for (auto entity : view) {
        auto& mc = view.get<MachineComponent>(entity);
        if (static_cast<int32_t>(mc.x) == x &&
            static_cast<int32_t>(mc.y) == y &&
            static_cast<int32_t>(mc.z) == z) {
            auto& inv = view.get<InventoryContainer>(entity);
            inv.slots.clear();
            auto* slots = completed->result_slots();
            for (uint16_t i = 0; i < slots->size(); ++i) {
                auto* s = slots->Get(i);
                inv.slots.push_back({s->item_id(),
                                      static_cast<uint8_t>(s->count()),
                                      s->meta()});
            }
            spdlog::info("[SimCore] Applied recipe result at ({},{},{})", x, y, z);
            break;
        }
    }
}

} // namespace simcore
