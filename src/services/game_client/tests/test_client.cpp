// Compile-time item ID invariants for the add-tree-generation change.
// Recipes are server-authoritative (ServerRecipeDB); the client no longer
// hardcodes recipe tables.
#include <common/ItemId.h>

static_assert(ItemId::pack("0:10:11:2") == 0x5802u, "oak_log id");
static_assert(ItemId::pack("0:10:11:3") == 0x5803u, "oak_leaves id");
static_assert(ItemId::pack("0:10:11:0") == 0x5800u, "chest id");
static_assert(ItemId::pack("0:10:11:1") == 0x5801u, "crafting_table id");
static_assert(ItemId::pack("0:10:00:0") == 0x4000u, "oak_planks id");

// Wire-contract check for the recipe UX gating change: Protocol::RecipeInfo
// carries unlock_era (quest era required to see the recipe). ServerRecipeDB's
// ParseRecipeInfo mirrors this field verbatim, so validating the schema round-
// trip here pins the contract the client-side era filter depends on.
#include <recipe_generated.h>
#include <flatbuffers/flatbuffers.h>

#include <cstdio>
#include <cstdint>

int main() {
    printf("=== GameClient Item ID Static Asserts ===\n");
    printf("All static_asserts passed.\n");

    flatbuffers::FlatBufferBuilder builder;
    auto off = Protocol::CreateRecipeInfoDirect(
        builder, 0 /*machine_type*/, "ebf", "gtnh:ebf_iron_smelting",
        400 /*duration*/, 2 /*unlock_era*/, nullptr /*inputs*/,
        nullptr /*outputs*/, false /*has_pattern*/, nullptr /*pattern*/);
    builder.Finish(off);
    auto* info = flatbuffers::GetRoot<Protocol::RecipeInfo>(builder.GetBufferPointer());
    if (info && info->unlock_era() == 2) {
        printf("unlock_era wire round-trip OK\n");
        return 0;
    }
    printf("FAIL: unlock_era wire round-trip\n");
    return 1;
}
