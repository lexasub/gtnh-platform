#include "TransformerSystem.h"
#include <common/ItemId.h>
#include <spdlog/spdlog.h>
#include <cmath>

namespace simcore {

namespace {
    // Voltage per tier: 0=ULV(8), 1=LV(32), 2=MV(128), 3=HV(512), 4=EV(2048)
    inline int32_t tierVoltage(uint8_t tier) {
        return static_cast<int32_t>(std::pow(4, tier)) * 8;
    }

    inline int32_t tierAmp(uint8_t) {
        return 1; // single amp packets for transformers
    }
}

bool TransformerSystem::isTransformer(uint16_t block_id) {
    return block_id == ItemId::pack("1110:11:0")  // transformer_mv_hv
        || block_id == ItemId::pack("1110:11:1"); // transformer_hv_ev
}

TransformerSystem::TransformerSystem(entt::registry& reg,
                                     std::shared_ptr<IEventPublisher> events,
                                     std::shared_ptr<PipeEnergyClient> pipeClient)
    : reg_(reg), events_(events), pipeClient_(pipeClient)
{}

void TransformerSystem::tick(float /*dt*/) {
    auto view = reg_.view<Block, Position, TransformerComponent, EnergyStorage>();

    for (auto ent : view) {
        auto& block = view.get<Block>(ent);
        auto& pos = view.get<Position>(ent);
        auto& tf = view.get<TransformerComponent>(ent);
        auto& energy = view.get<EnergyStorage>(ent);

        if (!isTransformer(block.id)) continue;

        int32_t inputVoltage = tierVoltage(tf.inputTier);
        int32_t outputVoltage = tierVoltage(tf.outputTier);

        if (tf.stepUp) {
            // Low→High: drain input buffer, accumulate then output high-voltage packets
            int32_t toConsume = std::min(energy.current, tf.maxInput);
            int32_t packets = toConsume / inputVoltage;

            if (packets > 0 && tf.buffer < tf.maxOutput * 4) {
                int32_t consume = packets * inputVoltage;
                // Output: combine N low-voltage packets into fewer high-voltage packets
                int32_t ratio = outputVoltage / inputVoltage;
                int32_t outPackets = packets / ratio;
                int32_t outEnergy = outPackets * outputVoltage;

                energy.current -= consume;
                tf.buffer += outEnergy;

                if (tf.buffer >= outputVoltage) {
                    int32_t sendPackets = tf.buffer / outputVoltage;
                    int32_t sendEnergy = sendPackets * outputVoltage;
                    tf.buffer -= sendEnergy;

                    if (pipeClient_) {
                        pipeClient_->publishNodeUpdate(
                            static_cast<uint64_t>(ent), pos.x, pos.y, pos.z,
                            energy.current, energy.capacity,
                            energy.maxInput, energy.maxOutput,
                            tf.outputTier,
                            static_cast<int32_t>(energy.type),
                            false, true  // is_source=false, is_sink=true
                        );
                    }

                    spdlog::trace("Transformer step-up (tier {}→{}): sent {} EU at {} V",
                                  tf.inputTier, tf.outputTier, sendEnergy, outputVoltage);
                }
            }
        } else {
            // High→Low: drain input high-voltage packets, distribute as low-voltage
            int32_t toConsume = std::min(energy.current, tf.maxInput);
            int32_t packets = toConsume / inputVoltage;

            if (packets > 0) {
                int32_t outEnergy = packets * inputVoltage;
                int32_t energyToAdd = outEnergy;

                int32_t space = energy.capacity - energy.current;
                int32_t actualAdd = std::min(energyToAdd, space);
                energy.current += actualAdd;

                // The energy is distributed as low-voltage packets
                spdlog::trace("Transformer step-down (tier {}→{}): distributed {} EU at {} V",
                              tf.inputTier, tf.outputTier, actualAdd, outputVoltage);
            }
        }
    }
}

} // namespace simcore
