# Change: File-based texture system with CSV mappings

## Why
Textures are currently procedurally generated (hardcoded colors in C++). Artists cannot add block textures without recompiling. The system needs file-based textures with per-face mappings and transparency support.

## What Changes
- Add `data/textures/` directory with source PNG packs (often.png, machines.png, ores.png, pipes.png)
- Add `data/textures/textures.csv` — maps tile ID to source file + tile coordinates
- Add `data/textures/block_faces.csv` — maps block ID + face to tile ID, with transparency flag
- Replace procedural `TextureAtlas::Init()` with file-based atlas builder: load source PNGs via lodepng, composite into single GPU atlas
- Replace `FillTileFlat`/`FillTileChecker` with `lodepng::decode()` + pixel copy
- Add transparency pass in `RenderScene`: blocks with `transparent=1` render with alpha blend after opaque pass
- No changes to vertex format, shaders, ChunkMeshBuilder, or protocol

## Impact
- Affected specs: `texture-system` (new capability)
- Affected code:
  - `src/services/game_client/RenderLib/Utils/TextureAtlas.h/.cpp` — rewrite Init, add CSV loader
  - `src/services/game_client/RenderLib/Scene/RenderScene.cpp` — add transparent render pass
  - `src/services/game_client/RenderLib/Common/RenderAPI.cpp` — pass `DATA_DIR` to atlas
  - `data/textures/*` — new asset files
  - `src/services/game_client/RenderLib/CMakeLists.txt` — link `lodepng` if not already
