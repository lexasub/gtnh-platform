#pragma once

#include "IUIWindow.h"
#include "Common/Types.h"

// ──────────────────────────────────────────────────────────────────────────────
// BlockAttachedWindow — intermediate base for windows bound to a world block.
//
// Provides:
//   - Anchor position (where the block is in the world)
//   - Network routing by position (OnNetworkUpdate checks coordinates)
//   - Automatic IsBlockAttached() = true
//
// Inherit from this for workbench, furnace, macerator, chest, etc.
// ──────────────────────────────────────────────────────────────────────────────
class BlockAttachedWindow : public IUIWindow {
public:
    explicit BlockAttachedWindow(BlockPos pos) : pos_(pos) {}

    BlockPos GetAnchorPos() const { return pos_; }
    void SetAnchorPos(BlockPos pos) { pos_ = pos; }

    bool IsBlockAttached() const final { return true; }

protected:
    BlockPos pos_;
};
