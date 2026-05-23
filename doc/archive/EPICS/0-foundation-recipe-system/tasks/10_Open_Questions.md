### Open Questions

1. **JSON vs FlatBuffers** — Whether to store recipes in JSON or FlatBuffers for serialization.
2. **Binary format** — Whether to store recipes in FlatBuffers for zero-copy IPC.
3. **Temperature source** — How RecipeManager obtains current temperature (RPC from machine?)
4. **Liquid volume** — How RecipeManager determines liquid volume in machine
5. **Purity measurement** — How RecipeManager determines current machine purity
6. **Biome representation** — How to represent and query biomes
7. **Complex conditions** — How to implement `special_condition` for player/object checks
8. **Category extension** — How to add new recipe categories without breaking changes