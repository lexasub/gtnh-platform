#pragma once
#include <entt/entt.hpp>
#include <unordered_map>
#include <cstdint>
#include <vector>
#include <memory>
#include <functional>
#include "components/Position.h"
#include "components/Block.h"
#include "components/MachineComponent.h"
#include "components/RecipeProgress.h"
#include "components/InventoryContainer.h"
#include "components/EnergyStorage.h"
#include "components/HeatIntakeComponent.h"
#include "components/MultiblockController.h"
#include "Systems/ISystem.h"
#include "MachineRegistry.h"

namespace simcore {

    // Pattern for electrolyser multiblock (3x3x3 minus corners? – keep as original)
    extern const std::vector<std::tuple<int32_t, int32_t, int32_t>> ELECTROLYSER_PATTERN;

    class SimulationEngine {
public:
    SimulationEngine() = default;
    void setMachineRegistry(const MachineRegistry* reg) { machine_registry_ = reg; }

    // Fired when a managed_externally machine entity is created (multiblock).
    // Main.cpp wires this to publishBlockEntityUpdate for reciped.
    std::function<void(int32_t x, int32_t y, int32_t z, uint16_t machine_id)> onMachineCreated;

    void onBlockChanged(uint32_t x, uint32_t y, uint32_t z,
                        uint16_t block_id, uint8_t meta, uint32_t mb_id);

        void registerSystem(std::unique_ptr<ISystem> system);
        void tickAll(float dt);

        uint64_t matchElectrolyser(uint32_t anchor_x, uint32_t anchor_y,
                                   uint32_t anchor_z, uint64_t controller_id);

        void registerController(uint64_t id, uint32_t x, uint32_t y, uint32_t z,
                                const std::vector<uint32_t>& blocks);

        bool isControllerActive(uint64_t id) const;
        const MultiblockController& getController(uint64_t id) const;
        void unregisterController(uint64_t id);

        entt::registry& reg() { return reg_; }

    private:
        bool isMachineBlock(uint16_t block_id) const;
        uint32_t defaultMachineSlotCount(uint16_t block_id) const;

        entt::entity findEntityAt(uint32_t x, uint32_t y, uint32_t z) const;
        void removeBlockFromController(uint32_t mb_id, uint32_t x, uint32_t y, uint32_t z);

        entt::registry reg_;
        std::unordered_map<uint64_t, MultiblockController> controllers_;
        std::vector<std::unique_ptr<ISystem>> systems_;
        uint64_t next_machine_id_ = 1;
        const MachineRegistry* machine_registry_ = nullptr;
    };

} // namespace simcore