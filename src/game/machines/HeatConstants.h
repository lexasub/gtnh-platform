#pragma once
#include <cstdint>
#include <engine/registry/ItemId.h>
namespace simcore {
namespace HeatConstants {
constexpr float OVERHEAT_WARNING_THRESHOLD = 0.90f;
constexpr float OVERHEAT_CRITICAL_THRESHOLD = 1.00f;
constexpr float ENVIRONMENT_COOLING_RATE = 4.0f;
constexpr float WATER_COOLING_MULTIPLIER = 3.0f;
constexpr uint32_t EXPLOSION_DELAY_TICKS = 60;
// gp-dd5q — explosion blast geometry.
//
// EXPLOSION_RADIUS is the outermost reach of the blast, in blocks, measured as
// Euclidean distance from the epicentre (squared distance is what the system
// actually compares, so no sqrt and no cubical/Manhattan approximation).
//
// The blast has two reach tiers, both <= EXPLOSION_RADIUS:
//   * EXPLOSION_ANCHOR_REACH       — multiblock controller anchors (mb_id != 0)
//   * EXPLOSION_SINGLEBLOCK_REACH  — single-block machines (mb_id == 0)
// Anchors are reinforced, so they take the shorter reach. This is the falloff:
// a block is destroyed iff its distance from the epicentre is within the reach
// tier for its own kind.
//
// Blast visibility is an ECS limitation, not a gameplay choice: ExplosionSystem
// has no chunk/block repository, so it can only clear blocks it can SEE, i.e.
// entities carrying MachineComponent + Position in the registry. Plain blocks and
// unloaded chunks survive every blast. See openspec/specs/explosion-mechanics.
constexpr int32_t EXPLOSION_RADIUS = 3;
constexpr int32_t EXPLOSION_ANCHOR_REACH = 2;
constexpr int32_t EXPLOSION_SINGLEBLOCK_REACH = 3;
constexpr uint32_t COOLANT_COOLING_AMOUNT = 50;
constexpr uint16_t COOLANT_ITEM_ID = ItemId::pack("0:11111:4");
constexpr int32_t CONVERSION_RATE = 1;
// HEAT sink keep-filled target for pipe-fed boilers: the boiler pulls from the
// pipe network (EnergyConsumeReq) to keep heat_stored near this level. At 20 Hz
// sim ticks and CONVERSION_RATE=1, 100 units ≈ 5 s of conversion headroom.
constexpr int32_t HEAT_SINK_REPLENISH_TARGET = 100;
} // namespace HeatConstants
} // namespace simcore
