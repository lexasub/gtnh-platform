// Compile-time item ID invariants for the add-tree-generation change.
// Recipes are server-authoritative (ServerRecipeDB); the client no longer
// hardcodes recipe tables.
#include <common/ItemId.h>

static_assert(ItemId::pack("0:10:11:2") == 0x5802u, "oak_log id");
static_assert(ItemId::pack("0:10:11:3") == 0x5803u, "oak_leaves id");
static_assert(ItemId::pack("0:10:11:0") == 0x5800u, "chest id");
static_assert(ItemId::pack("0:10:11:1") == 0x5801u, "crafting_table id");
static_assert(ItemId::pack("0:10:00:0") == 0x4000u, "oak_planks id");

#include <cstdio>
int main() {
    printf("=== GameClient Item ID Static Asserts ===\n");
    printf("All static_asserts passed.\n");
    return 0;
}
