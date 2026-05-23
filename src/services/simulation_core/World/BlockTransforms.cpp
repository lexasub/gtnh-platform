#include "World/BlockTransforms.h"
#include <optional>

namespace simcore {

    std::optional<TransformResult> applyBlockTransform(
        uint16_t expected_block_id,
        uint16_t new_block_id,
        uint8_t /*new_meta*/)  // мета в текущей реализации не используется
    {
        // Правило: камень (1) на траве (2) → тропинка (3)
        if (new_block_id == 1 && expected_block_id == 2) {
            return TransformResult{3, 0};
        }

        // Другие трансформации можно добавить здесь
        // ...

        return std::nullopt; // без изменений
    }

} // namespace simcore