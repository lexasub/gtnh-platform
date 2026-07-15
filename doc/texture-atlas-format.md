# Texture Atlas Format

## `data/textures/textures.csv` — Tile Registry

Maps tile IDs to source PNG files and positions.

```
tile_id,filename,tile_x,tile_y,rotate
```

| Column    | Type    | Description |
|-----------|---------|-------------|
| `tile_id` | int     | Unique tile identifier (0–255) |
| `filename`| string  | PNG file path relative to `data/textures/` |
| `tile_x`  | int     | Tile column in source PNG (0-based, `pixel_x = tile_x * 16`) |
| `tile_y`  | int     | Tile row in source PNG (0-based, `pixel_y = tile_y * 16`) |
| `rotate`  | int     | (Optional) Rotation: 0=0°, 1=90°CW, 2=180°, 3=270°CW. Default 0 |

**Example:**
```
0,often.png,0,0,0
1,often.png,0,1,0
16,machines.png,2,1,1
```

### Atlas slot assignment

Slots are assigned automatically in registration order: first tile → atlas `(0,0)`, second → `(1,0)`, ..., `(15,0)`, `(0,1)`, etc. Source PNG positions (`tile_x`, `tile_y`) are only used for reading pixels — they do not control atlas layout.

Supported PNG sizes: 16×16, 32×32, 48×48, 64×64, etc. — dimensions must be multiples of 16. Each 16×16 block in the PNG is referenced by `(tile_x, tile_y)`.

### Rotation

Pixels are rotated at load time (once, baked into atlas). The rotation is CW around the tile center. Applied before copying to atlas — no runtime cost.

---

## `data/textures/block_faces.csv` — Block Face Mapping

Maps block IDs to tile IDs per face.

```
block_id,face_px,face_nx,face_py,face_ny,face_pz,face_nz,transparent
```

| Column       | Type | Description |
|--------------|------|-------------|
| `block_id`   | int  | Block identifier (0–255) |
| `face_px`    | int  | Tile ID for +X face (right) |
| `face_nx`    | int  | Tile ID for −X face (left) |
| `face_py`    | int  | Tile ID for +Y face (top) |
| `face_ny`    | int  | Tile ID for −Y face (bottom) |
| `face_pz`    | int  | Tile ID for +Z face (front) |
| `face_nz`    | int  | Tile ID for −Z face (back) |
| `transparent`| int  | 1 = alpha-blended rendering, 0 = opaque |

**Example:**
```
1,0,0,0,0,0,0,0        # stone: all faces tile 0, opaque
2,16,16,16,16,16,16,0  # cobblestone: all faces tile 16, opaque
3,3,3,2,1,3,3,0        # grass: top=tile 2, bottom=tile 1, sides=tile 3
14,4,4,4,4,4,4,1       # glass: tile 4, transparent
```

Blocks without an entry use default mapping: `block_id → tile_id` (block N uses tile N). Unregistered tile IDs show magenta/black checkerboard.

---

## Atlas Texture

- Size: 256×256 RGBA (256 KB)
- Tile size: 16×16 pixels
- Grid: 16 columns × 16 rows = 256 slots
- Format: `bgfx::TextureFormat::RGBA8`
- Filtering: bilinear with half-pixel UV inset (prevents tile bleeding)

## `data/textures/item_icons.csv` — Item Icon Mapping (PLANNED)

Maps item IDs to a single tile ID for inventory/UI rendering.

```
item_id,tile_id
```

| Column    | Type   | Description |
|-----------|--------|-------------|
| `item_id` | string | Item identifier (same format as `items.csv`) |
| `tile_id` | int    | Tile ID in the atlas (references `textures.csv`) |

**Example:**
```
0:0:1,0          # stone → tile 0 (block texture reused)
0:110:1,16       # iron_ingot → tile 16 (dedicated icon sprite)
```

### Resolution chain

Item icons resolve in order:
1. `item_icons.csv` — explicit mapping (highest priority)
2. `block_faces.csv` — first face (`face_px`) of matching block_id (free for all blocks)
3. Fallback — magenta/black checkerboard placeholder

This means blocks, ores, and wood items get icons automatically from `block_faces.csv` without explicit entries. Only non-block items (ingots, tools, machines, cables, pipes, fluids) need entries in `item_icons.csv`.

### What needs art (~60 items)

| Category | Count | Examples |
|----------|-------|---------|
| Blocks/ores/wood | ~29 | stone, iron_ore, oak_planks — auto from `block_faces` |
| Ingots/dusts/materials | ~15 | iron_ingot, tin_dust, bronze_plate |
| Tools | ~9 | drill_lv, chainsaw_lv, wrench |
| Machines | ~17 | heat_macerator, steam_compressor, creative_generator |
| Cables/pipes | ~10 | cable_copper, fluid_pipe, item_pipe |
| Buckets/fluids | ~8 | water_bucket, sulfuric_acid_bucket |

All icons are 16×16 px pixel art (same as block tiles). Items in NEI/GTNH are 2D sprites, not 3D renders.

---

## `data/textures/textures_merge.csv` — Texture Compositing

Overlays one tile on top of another at atlas load time (CPU, one-shot). Useful for ores on different stone types, decorative variants, etc.

```
composite_id,base_tile_id,overlay_tile_id
```

| Column         | Type | Description |
|----------------|------|-------------|
| `composite_id` | int  | New tile_id for the composited result (must not collide with existing tiles) |
| `base_tile_id` | int  | tile_id from `textures.csv` (bottom layer, e.g. stone) |
| `overlay_tile_id` | int | tile_id from `textures.csv` (top layer with alpha, e.g. ore sprite) |

**Example:**
```
composite_id,base_tile_id,overlay_tile_id
20,0,1          # often.png(0,0) + often.png(0,1) → composite tile 20
21,2,1          # often.png(0,2) + often.png(0,1) → composite tile 21
22,3,1          # often.png(0,3) + often.png(0,1) → composite tile 22
```

### How it works

1. Base tiles are loaded into the atlas first (from `textures.csv`)
2. `textures_merge.csv` is read after base tiles
3. For each entry: overlay tile's source PNG is re-read, alpha-blended onto the base tile in the atlas (`src_over` compositing)
4. Result is placed in the next free atlas slot; `composite_id` is registered in tile→atlas mapping
5. Checkerboard fill skips all used slots (base + composite)

### Usage

Reference `composite_id` in `block_faces.csv` or `item_icons.csv` like any regular tile_id. The composited tile is indistinguishable from a hand-drawn tile.

**Typical use case** (GTNH ores):
```
# textures_merge.csv — ore overlays
20,0,1    # stone + copper_ore
21,2,1    # netherrack + copper_ore
22,3,1    # endstone + copper_ore
```
Then in `block_faces.csv`:
```
200,20,20,20,20,20,20,0   # copper_ore uses composite tile 20
```

---

## Atlas Strategy

**Single atlas, always resident.** All block tiles + item icons share one atlas.

| Atlas size | Capacity | VRAM | Use case |
|------------|----------|------|----------|
| 256×256 | 256 tiles | 256 KB | **Current** — sufficient for blocks + items |
| 512×512 | 1024 tiles | 1 MB | When machines/animations grow beyond 256 |
| Texture2DArray | Unlimited | per-layer | If atlas packing becomes a bottleneck |

**Caching/eviction**: None needed. 256 KB fits in VRAM permanently. No LRU, no tiered eviction. Revisit if animated textures or tile entity sprites are added.

---

## Limitations

### Atlas capacity

Current atlas is 256×256 = 256 unique tile slots. For a GTNH-scale game with hundreds of machines and blocks this will not be enough.

**TODO**: increase capacity — either bump atlas to 512×512 (1024 tiles) or switch to `Texture2DArray` (unlimited layers, no packing overhead).

See also: `kDefaultTilesX` / `kDefaultTilesY` in `TextureAtlas.h`, and all per-tile arrays sized `[256]` in `TextureAtlas.cpp`.

## Implementation

- `src/services/game_client/RenderLib/Utils/TextureAtlas.cpp` — atlas builder (blocks + items)
- `src/services/game_client/UI/Components/ItemColor.h` — **TO BE REPLACED** with UV lookup from atlas
- `src/services/game_client/Render/ChunkMeshBuilder.cpp` — mesh builder (UV from face-local axes)
- `shaders/fs_block.sc` — fragment shader (`texture2D(s_texAtlas, v_texcoord) * v_color`)
