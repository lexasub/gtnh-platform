#include "Network/RouterEventPublisher.h"
#include "Network/clients/IoUringRouterClient.h"
#include "core_generated.h"
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

namespace simcore {

RouterEventPublisher::RouterEventPublisher(std::shared_ptr<IoUringRouterClient> router)
    : router_(std::move(router))
{}

void RouterEventPublisher::publishBlockAck(uint8_t status,
                                           int32_t x, int32_t y, int32_t z,
                                           uint16_t block_id, uint8_t meta,
                                           const char* reason,
                                           uint32_t request_id)
{
    flatbuffers::FlatBufferBuilder builder(128);
    auto pos = Protocol::Vec3i(x, y, z);
    flatbuffers::Offset<flatbuffers::String> reason_off = 0;
    if (reason && reason[0] != '\0') {
        reason_off = builder.CreateString(reason);
    }
    auto ack = Protocol::CreateBlockAck(builder, &pos,
                                        static_cast<Protocol::BlockAckStatus>(status),
                                        block_id, meta, reason_off, request_id);
    builder.Finish(ack);
    std::vector<uint8_t> ack_data(builder.GetBufferPointer(),
                                  builder.GetBufferPointer() + builder.GetSize());
    router_->Publish("player.actions.ack", ack_data);
    spdlog::debug("Published BlockAck: status={} at ({},{},{}) id={} request_id={}", status, x, y, z, block_id, request_id);
}

void RouterEventPublisher::publishBlockChangedEvent(int32_t x, int32_t y, int32_t z,
                                                    uint16_t block_id, uint8_t meta)
{
    flatbuffers::FlatBufferBuilder builder(128);
    auto pos = Protocol::Vec3i(x, y, z);
    auto event = Protocol::CreateBlockChangedEvent(builder, &pos, block_id, meta, 0);
    builder.Finish(event);
    std::vector<uint8_t> event_data(builder.GetBufferPointer(),
                                    builder.GetBufferPointer() + builder.GetSize());
    router_->Publish("world.blocks.changed", event_data);
    spdlog::debug("Published BlockChangedEvent: id={} at ({},{},{})", block_id, x, y, z);
}

void RouterEventPublisher::publishBlockEntityUpdate(int32_t x, int32_t y, int32_t z,
                                                       uint16_t machine_id,
                                                       const std::vector<uint8_t>& inventory_data,
                                                       float progress,
                                                       uint32_t energy,
                                                       EnergyType energy_type,
                                                       uint32_t energy_capacity,
                                                       int slots_in,
                                                       float heat_ratio)
{
    flatbuffers::FlatBufferBuilder builder(128);

    auto pos = Protocol::Vec3i(x, y, z);

    // Build inventory items from raw struct bytes (each slot = 5 bytes packed)
    constexpr size_t kSlotSize = sizeof(uint16_t) + sizeof(uint8_t) + sizeof(uint16_t); // 5 bytes
    std::vector<Protocol::ItemStack> input_items;
    std::vector<Protocol::ItemStack> output_items;
    if (inventory_data.size() >= kSlotSize) {
        size_t num_slots = inventory_data.size() / kSlotSize;
        // If slots_in not provided, all items go to input_items (backward compat)
        size_t split = (slots_in < 0) ? num_slots : static_cast<size_t>(slots_in);
        for (size_t i = 0; i < num_slots; ++i) {
            size_t off = i * kSlotSize;
            uint16_t item_id = *reinterpret_cast<const uint16_t*>(&inventory_data[off]);
            uint8_t  count   = inventory_data[off + 2];
            uint16_t meta    = *reinterpret_cast<const uint16_t*>(&inventory_data[off + 3]);
            if (i < split)
                input_items.emplace_back(item_id, count, meta);
            else
                output_items.emplace_back(item_id, count, meta);
        }
    }

    auto update = Protocol::CreateBlockEntityUpdateDirect(
        builder,
        &pos,
        machine_id,
        progress,
        energy,
        energy_capacity,
        static_cast<Protocol::EnergyType>(energy_type),
        input_items.empty() ? nullptr : &input_items,
        output_items.empty() ? nullptr : &output_items,
        nullptr,                                       // fluid_tanks
        heat_ratio,                                    // temperature (used as heat_ratio for overheat warnings)
        0,                                             // network_id
        0,                                             // flags
        0,                                             // mb_id
        false,                                         // structure_valid
        nullptr,                                       // hatches
        nullptr                                        // covers
    );

    builder.Finish(update);
    std::vector<uint8_t> update_data(
        builder.GetBufferPointer(),
        builder.GetBufferPointer() + builder.GetSize()
    );
    router_->Publish("world.block_entity.update", update_data);
    spdlog::debug("Published BlockEntityUpdate: machine_id={} at ({},{},{}) progress={} energy={} type={}",
                  machine_id, x, y, z, progress, energy, static_cast<int>(energy_type));
}

void RouterEventPublisher::publishMachineSlotResponse(
    int32_t x, int32_t y, int32_t z,
    uint16_t slot_idx,
    bool success,
    uint16_t item_id,
    uint8_t count,
    uint16_t meta,
    const char* error)
{
    (void)item_id; (void)count; (void)meta;
    flatbuffers::FlatBufferBuilder fbb(128);
    auto pos = Protocol::Vec3i(x, y, z);
    auto err = error ? fbb.CreateString(error) : 0;
    auto resp = Protocol::CreateSetMachineSlotResp(
        fbb, &pos, static_cast<uint8_t>(slot_idx), success, err);
    fbb.Finish(resp);
    std::vector<uint8_t> data(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
    router_->Publish("player.machine.slot.response", std::move(data));
}

void RouterEventPublisher::publishMachineConfigUpdatedEvent(int32_t x, int32_t y, int32_t z,
                                                           const std::array<uint8_t, 6> &side_config)
{
    flatbuffers::FlatBufferBuilder builder(128);
    auto pos = Protocol::Vec3i(x, y, z);
    std::vector<uint8_t> faces(side_config.begin(), side_config.end());
    auto config = Protocol::CreateMachineConfigUpdatedDirect(builder, &pos, 0, 0, &faces);
    builder.Finish(config);
    std::vector<uint8_t> event_data(builder.GetBufferPointer(),
                                    builder.GetBufferPointer() + builder.GetSize());
    router_->Publish("world.machine.config.updated", event_data);
    spdlog::debug("Published MachineConfigUpdatedEvent: at ({},{},{}) side_config={}{}{}{}{}{}",
                  x, y, z, (int)side_config[0], (int)side_config[1], (int)side_config[2],
                  (int)side_config[3], (int)side_config[4], (int)side_config[5]);
}

} // namespace simcore
