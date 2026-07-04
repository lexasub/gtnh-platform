#pragma once
#include <cstdint>
#include <common/ItemId.h>

// IDs must match data/registry/items.csv
constexpr uint16_t BLOCK_ID_FLUID_PIPE       = ItemId::pack("1111:10:0");
constexpr uint16_t BLOCK_ID_ITEM_PIPE        = ItemId::pack("1111:10:1");
constexpr uint16_t BLOCK_ID_DENSE_ITEM_PIPE  = ItemId::pack("1111:10:2");
constexpr uint16_t BLOCK_ID_DENSE_FLUID_PIPE = ItemId::pack("1111:10:3");
