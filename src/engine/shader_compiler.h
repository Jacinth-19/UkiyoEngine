#pragma once

// Runtime GLSL -> SPIR-V compiler, backed by the glslang library linked into the engine.
// Generated shader graphs use it to become SPIR-V without an external glslangValidator.

#include <cstdint>
#include <string>
#include <vector>

namespace engine {

enum class ShaderStage { Vertex, Fragment };

struct SpirvResult {
    bool ok = false;
    std::vector<uint32_t> words;  // SPIR-V, valid when ok is true
    std::string log;              // compiler messages (errors and warnings)
};

// Compiles GLSL 450 to SPIR-V for Vulkan 1.3. The source uses Vulkan semantics (layout set and
// binding). Call from one thread at a time.
SpirvResult compileGlslToSpirv(const std::string& source, ShaderStage stage);

} // namespace engine
