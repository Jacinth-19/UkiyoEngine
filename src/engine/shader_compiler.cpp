#include "engine/shader_compiler.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <mutex>

namespace engine {

namespace {

// glslang must be initialised once per process before the first shader is parsed.
void ensureInitialised()
{
    static std::once_flag once;
    std::call_once(once, [] { glslang::InitializeProcess(); });
}

} // namespace

SpirvResult compileGlslToSpirv(const std::string& source, ShaderStage stage)
{
    ensureInitialised();
    SpirvResult result;

    const EShLanguage lang = stage == ShaderStage::Vertex ? EShLangVertex : EShLangFragment;
    glslang::TShader shader(lang);
    const char* text = source.c_str();
    shader.setStrings(&text, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);

    const EShMessages messages = static_cast<EShMessages>(EShMsgDefault | EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 450, ENoProfile, false, false, messages)) {
        result.log = std::string(shader.getInfoLog()) + shader.getInfoDebugLog();
        return result;
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        result.log = std::string(program.getInfoLog()) + program.getInfoDebugLog();
        return result;
    }

    glslang::GlslangToSpv(*program.getIntermediate(lang), result.words);
    result.ok = true;
    return result;
}

} // namespace engine
