## 1. Implementation
- [x] 1.1 In `PipeNetworkService::handleBlockChanged`, populate `pipe_meta_[key] = event->meta()` for all `isPipeBlock` blocks (currently gated inside `if (isCableBlock(...))`).
- [x] 1.2 Add `uint8_t meta` field to `PipeNode` (`PipeNetwork.h`); set it when a pipe node is registered/updated.
- [x] 1.3 Add a canonical face-offset table indexed by meta bit and a `metaBitSet(meta, f)` helper (`meta==0 ⇒ 0x3F`); replace the item fallback scan's `dx/dy/dz` ordering with it.
- [x] 1.4 In `handleItemNodeUpdate` fallback scan, skip `addEdge` unless `metaBitSet(fromMeta, f) && metaBitSet(toMeta, f^1)`.
- [x] 1.5 Add pipe↔pipe edge creation for item/fluid on node registration / `rebuildItemNetworks`, applying the same mask.
- [x] 1.6 Add a masked fallback scan to `handleFluidNodeUpdate` (fluid currently builds no edges).
- [x] 1.7 Build (`cd cmake-build-debug && ninja -j5`) + run `ctest --output-on-failure -j$(nproc)`.
- [x] 1.8 Add/extend a unit test for per-face masking on item/fluid edges (mirror the cable mask test).
