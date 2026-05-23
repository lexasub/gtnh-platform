#pragma once

#include <string>
#include <bgfx/bgfx.h>

namespace renderlib {

    const bgfx::Memory* LoadBinaryShader(const std::string& path, const char* label);

} // namespace renderlib