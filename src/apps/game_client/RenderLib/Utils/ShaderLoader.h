#pragma once

#include <bgfx/bgfx.h>
#include <string>

namespace renderlib {

const bgfx::Memory *LoadBinaryShader(const std::string &path,
                                     const char *label);

} // namespace renderlib