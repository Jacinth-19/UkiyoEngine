// The runtime GLSL compiler, on a platform that has none.
//
// Shader graphs are compiled to SPIR-V when a material is edited, by glslang linked into the engine. The NDK does not
// ship glslang and the engine does not vendor it, so on Android a graph material cannot be built. Everything a shipped
// game ships is already compiled (the shaders are in the package, and the models carry their own materials), so this is
// only reachable from an editor feature that has no Android editor.
#include "engine/shader_compiler.h"

namespace engine {

SpirvResult compileGlslToSpirv(const std::string&, ShaderStage) {
    SpirvResult r;
    r.ok = false;
    r.log = "this build has no GLSL compiler: generate the material on the desktop and ship the compiled result";
    return r;
}

} // namespace engine
