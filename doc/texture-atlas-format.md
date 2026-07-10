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

## Limitations

### Atlas capacity

Current atlas is 256×256 = 256 unique tile slots. For a GTNH-scale game with hundreds of machines and blocks this will not be enough.

**TODO**: increase capacity — either bump atlas to 512×512 (1024 tiles) or switch to `Texture2DArray` (unlimited layers, no packing overhead).

See also: `kDefaultTilesX` / `kDefaultTilesY` in `TextureAtlas.h`, and all per-tile arrays sized `[256]` in `TextureAtlas.cpp`.

## Implementation

- `src/services/game_client/RenderLib/Utils/TextureAtlas.cpp` — atlas builder
- `src/services/game_client/Render/ChunkMeshBuilder.cpp` — mesh builder (UV from face-local axes)
- `shaders/fs_block.sc` — fragment shader (`texture2D(s_texAtlas, v_texcoord) * v_color`)
