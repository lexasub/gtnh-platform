# Task 4: Height Layers per Ore Type

## Objective
Implement height-based filtering so each ore type only generates within its configured Y-range. This ensures coal appears high, diamonds deep, etc.

## Requirements

### 4.1 Height range filter
**Location**: `src/services/world_generator/OreGenerator.cpp`

```cpp
bool OreGenerator::isInHeightRange(const OreDef& ore, int32_t y) const {
    return y >= ore.min_y && y <= ore.max_y;
}
```

### 4.2 Multi-layer generation loop
The generation loop from Task 3 already iterates `y = ore.min_y` to `ore.max_y`. Verify:
- Each ore type only generates within its configured range
- Overlapping ranges (e.g., iron and coal overlap at y=5-40) create mixed deposits

```cpp
// In generateOres():
for (const auto& ore : config.allOres()) {
    for (int32_t y = ore.min_y; y <= ore.max_y; y++) {
        // ... only stone replacement logic ...
    }
}
```

### 4.3 Height-dependent rarity
Some ores should become rarer at range edges:
```cpp
float OreGenerator::heightRarityFactor(const OreDef& ore, int32_t y) {
    float range = ore.max_y - ore.min_y;
    float midY = (ore.max_y + ore.min_y) / 2.0f;
    float distFromCenter = std::abs(y - midY) / (range / 2.0f);
    
    // Linear falloff: 1.0 at center, 0.3 at edges
    return 1.0f - (distFromCenter * 0.7f);
}
```

### 4.4 Expected ore distribution

| Ore | Y Range | Best Y | Relative Frequency |
|-----|---------|--------|-------------------|
| Coal | 5-80 | 40-60 | Very common |
| Iron | 5-40 | 20-35 | Common |
| Copper | 10-60 | 25-45 | Common |
| Tin | 10-50 | 20-35 | Common |
| Gold | 5-30 | 12-20 | Uncommon |
| Redstone | 5-20 | 8-15 | Uncommon |
| Lapis | 5-25 | 10-18 | Rare |
| Diamond | 5-15 | 8-12 | Very rare |

## Acceptance Criteria
- [ ] `isInHeightRange()` correctly filters ore generation by Y
- [ ] Coal appears at all heights up to y=80
- [ ] Diamond ONLY appears below y=15
- [ ] Gold UNCOMMON below y=30
- [ ] Height rarity falloff works (rarer at edges of range)
- [ ] Overlapping ranges create mixed zones (e.g., iron + coal at y=10-40)
- [ ] Performance: height check is O(1) per block — no measurable overhead

## Dependencies
- Task 2 (ore config — min_y/max_y values)
- Task 3 (ore generation loop)
- Required by: Task 5 (WorldGenerator integration)

## Files to Modify
- `src/services/world_generator/OreGenerator.cpp` — height filter + rarity falloff
- No new files needed
