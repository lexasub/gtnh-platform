# Change: Expand texture atlas capacity

## Why

The atlas has reached tile ID 255, while the current runtime still has a 16×16 slot grid and 8-bit slot accounting. The current source and merge registries also allocate redundant standalone slots for self-merge overlays, which exceeds the existing physical capacity. New item artwork needs additional safe logical IDs and runtime slots.

## What Changes

- Expand the runtime atlas from 256×256 to 512×512 RGBA8 while retaining 16×16-pixel tiles (32×32 slots).
- Support logical tile IDs 0–1023 consistently in the runtime, additive scanner, and documentation.
- Guard all registry tile references and physical slot allocations against out-of-range IDs/capacity exhaustion.
- Avoid allocating a standalone atlas slot for self-merge overlay cells while retaining their source-image coordinates for composition.

## Impact

- Affected capability: `texture-system`.
- Affected code: `TextureAtlas`, the additive texture scanner and tests, texture-atlas format documentation.
- GPU memory for the atlas increases from 256 KiB to 1 MiB. Existing tile size, source PNG format, UV convention, and normal merge behavior remain unchanged.
