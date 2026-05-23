#include "ShaderLoader.h"
#include <fstream>
#include <spdlog/spdlog.h>

namespace renderlib {

    const bgfx::Memory* LoadBinaryShader(const std::string& path, const char* label) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            spdlog::error("Failed to open {}: {}", label, path);
            return nullptr;
        }
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        const bgfx::Memory* mem = bgfx::alloc(static_cast<uint32_t>(size));
        if (!file.read(reinterpret_cast<char*>(mem->data), size)) {
            spdlog::error("Failed to read {}: {}", label, path);
            return nullptr;
        }
        return mem;
    }

} // namespace renderlib