#pragma once

#include "Render/BlockRenderRegistry.h" // blockIdToPipeType
#include "Render/PipeMeta.h"            // FaceMask, META_BIT_TO_FACEMASK

#include "Crafting/ClientItemRegistry.h"       // ItemRegistry::GetName
#include "Network/ResourceBufferStateStore.h"  // authoritative buffer state

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

// Pipe fluid overlay (openspec add-pipe-fluid-overlay) — pure logic extracted
// from GameClient/RenderBridge so it can be tested without launching the game
// (same rationale as Render/WrenchGrid.h for the wrench overlay). The overlay
// is strictly read-only: the gate and mask mapping never send requests, and
// the display text is built solely from ResourceBufferStateStore::FindAt().
namespace pipe_fluid_overlay {

// Fluid pipes covered by the overlay (design.md Decisions: no cable/heat).
inline constexpr bool IsFluidPipeType(PipeType type) {
  return type == PipeType::FLUID_PIPE || type == PipeType::DENSE_FLUID_PIPE;
}

// Overlay gate: toggle on + highlight on a fluid-pipe block.
inline bool ShouldShowOverlay(bool toggleOn, bool hasHighlight,
                              uint16_t highlightedBlockId) {
  return toggleOn && hasHighlight && highlightedBlockId != 0 &&
         ItemId::isPipe(highlightedBlockId) &&
         IsFluidPipeType(blockIdToPipeType(highlightedBlockId));
}

// FaceMask (FACE_* bits from detectConnections) → per-cube-face bools indexed
// in the META_BIT_TO_FACEMASK order {+X,-X,+Y,-Y,+Z,-Z} — one index works for
// the FrameExt array and the kFaceCorners screen-space projection.
inline void ConnectableFromMask(FaceMask mask, bool out[6]) {
  for (int i = 0; i < 6; ++i) out[i] = (mask & META_BIT_TO_FACEMASK[i]) != 0;
}

// Debug line for the targeted pipe. entry == nullptr (no current server
// snapshot: unknown/stale) → the explicit placeholder, never a fabricated
// zero (design.md Display semantics). The name resolves at display time via
// ItemRegistry::GetName; the store keeps only the canonical packed ID.
inline void FormatStateText(char* buf, size_t bufsize,
                            const ResourceBufferStateStore::Entry* entry) {
  if (!entry) {
    std::snprintf(buf, bufsize, "fluid: unknown/stale");
    return;
  }
  const std::string name(
      ItemRegistry::GetName(static_cast<uint16_t>(entry->resource_id)));
  std::snprintf(buf, bufsize, "fluid 0x%04X %s: %d / %d", entry->resource_id,
                name.c_str(), entry->amount, entry->capacity);
}

}  // namespace pipe_fluid_overlay
