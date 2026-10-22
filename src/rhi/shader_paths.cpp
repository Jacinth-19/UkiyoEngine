// The directory compiled shaders are loaded from. One string, because two backends and one player have to agree on it.
#include "rhi/rhi.h"

#include <utility>

namespace rhi {
namespace {

// UKIYO_SHADER_DIR is the build tree's shader output, set by CMake on the whole engine. A game that ships its own
// shaders replaces this before the device is created.
std::string g_root = UKIYO_SHADER_DIR;

} // namespace

void setShaderRoot(std::string dir) {
    if (!dir.empty()) g_root = std::move(dir);
}

const std::string& shaderRoot() { return g_root; }

} // namespace rhi
