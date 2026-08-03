# Change: Implement Ore Generation

## Why
World currently generates flat terrain (grass/stone/air). Ores are needed to test the machine processing chain (macerator → furnace → compressor). Player has no way to obtain raw materials.

## What Changes
- Add sinusoidal vein ore generation to WorldGenerator
- Register ore block IDs for iron, gold, tin, copper, coal, redstone, lapis, diamond
- Configure vein height ranges, density thresholds per ore type
- Wire ore generation into existing chunk generation pipeline

## Impact
- Affected specs: ore-generation (new)
- Affected code: world_generator (WorldGenerator.cpp), chunk_store (integration), data/registry (ore config)
