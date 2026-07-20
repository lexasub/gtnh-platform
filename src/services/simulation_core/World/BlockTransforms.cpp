#include "World/BlockTransforms.h"
#include <optional>
#include <common/ItemId.h>

namespace simcore {

    std::optional<TransformResult> applyBlockTransform(
        uint16_t expected_block_id,
        uint16_t new_block_id,
        uint8_t /*new_meta*/)  // мета в текущой реализации не используется
    {
        // Правило: камень (1) на траве (2) → тропинка (3)
        if (new_block_id == ItemId::pack("0:0:1") && expected_block_id == ItemId::pack("0:0:2")) {
            return TransformResult{ItemId::pack("0:0:3"), 0};
        }

        // Другие трансформации можно добавить здесь
        // ...

        return std::nullopt; // без изменений
    }

} // namespace simcore