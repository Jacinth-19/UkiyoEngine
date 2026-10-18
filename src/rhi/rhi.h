// Ukiyo RHI: a deliberately small rendering-hardware-interface shared by the
// OpenGL 4.5 and Vulkan 1.3 backends. The renderer only talks to this header,
// so every render pass is written once and runs on both APIs.
//
// Conventions (identical on both backends, see docs/CONVENTIONS.md):
//   * Clip space: OpenGL-style depth [0,1] (glm::*_ZO projections).
//   * Render-target memory: row 0 is the bottom of the image (GL convention).
//     The Vulkan backend flips this only when presenting to a real swapchain.
//   * Vertex layout is fixed (see rhi::Vertex) so meshes are backend-agnostic.
//   * Descriptor sets are typed (Frame / Material / Skin / Post) with fixed
//     binding numbers, declared in shaders/common.glsl.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

struct GLFWwindow;

namespace rhi {

enum class Backend { OpenGL, Vulkan };

enum class Format : uint8_t {
    RGBA8,        // UNORM 8-bit
    RGBA8_SRGB,   // sRGB 8-bit (hardware linearises on sample)
    RGBA16F,      // HDR colour target
    D32F,         // 32-bit float depth
    Backbuffer,   // whatever the presentation target uses
};

enum class BufferUsage : uint8_t { Vertex, Index, Uniform };
// ShaderMaterial: parameters of a generated (node graph) material. UBO at binding 15,
// textures at bindings 0-7. Vulkan only.
// Particles: instance UBO at binding 11 (vertex stage), used by the particle quads.
enum class SetType : uint8_t { Frame, Material, Skin, Post, ShaderMaterial, Motion, Particles };
enum class Cull : uint8_t { None, Back, Front };
enum class Blend : uint8_t { None, Alpha, Additive };
enum class Filter : uint8_t { Nearest, Linear };
enum class Wrap : uint8_t { Repeat, Clamp };

// The one vertex format every mesh uses. 96 bytes. The colour, the second UV set and the outline's smooth
// normal are new: they default to white, to the first UV set and to zero (zero = use the normal), so meshes
// that have none behave as before.
struct Vertex {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec2 uv;
    uint16_t joints[4];
    glm::vec4 weights;
    glm::vec4 color{1.0f};   // COLOR_0; white when the mesh has none
    glm::vec2 uv1{0.0f};     // TEXCOORD_1; the loader copies uv when the mesh has none
    glm::vec4 smoothNormal{0.0f};   // object-space normal averaged over shared positions (xyz); zero = none
};
static_assert(sizeof(Vertex) == 96, "Vertex layout is shared with shaders");

struct TextureDesc {
    uint32_t width = 1;
    uint32_t height = 1;
    Format format = Format::RGBA8;
    bool mipmaps = false;
    Filter filter = Filter::Linear;
    Wrap wrap = Wrap::Clamp;
};

struct DeviceConfig {
    uint32_t width = 1280;
    uint32_t height = 720;
    bool headless = false;      // render to an offscreen backbuffer
    bool validation = false;    // Vulkan validation layers if available
};

class Buffer {
public:
    virtual ~Buffer() = default;
    virtual void upload(const void* data, size_t size, size_t offset = 0) = 0;
    virtual size_t size() const = 0;
};

class Texture {
public:
    virtual ~Texture() = default;
    virtual uint32_t width() const = 0;
    virtual uint32_t height() const = 0;
    virtual Format format() const = 0;
};

class Pipeline {
public:
    virtual ~Pipeline() = default;
};

class DescriptorSet {
public:
    virtual ~DescriptorSet() = default;
    virtual void setBuffer(uint32_t binding, Buffer* buffer) = 0;
    virtual void setTexture(uint32_t binding, Texture* texture) = 0;
};

struct PipelineDesc {
    // Shader base name: loads <vertexShader>.vert and <fragmentShader>.frag
    std::string vertexShader;
    std::string fragmentShader;
    // Optional SPIR-V words for the fragment stage. When non-empty it replaces
    // fragmentShader (generated materials). Vulkan uses it.
    std::vector<uint32_t> fragmentSpirv;
    // Optional GLSL 450 text for the fragment stage (generated materials). GL compiles this text,
    // after removing the Vulkan `set = N,` qualifiers. Used when fragmentSpirv is empty.
    std::string fragmentGlsl;
    // Descriptor sets in order: index i in bindDescriptorSet(i, ...) matches sets[i].
    std::vector<SetType> sets;
    bool vertexInput = false;     // true -> Vertex layout, false -> fullscreen/procedural
    Cull cull = Cull::Back;
    bool depthTest = false;
    bool depthWrite = false;
    Blend blend = Blend::None;
    std::vector<Format> colorFormats;
    bool hasDepth = false;        // depth attachment (always D32F)
};

// Describes one render pass. Clear values apply when clearColor/clearDepth is set,
// otherwise existing contents are kept (load).
struct PassDesc {
    std::vector<Texture*> colors;
    Texture* depth = nullptr;
    bool backbuffer = false;      // render into the presentation target instead
    bool clearColor = true;
    glm::vec4 clearValue{0.0f, 0.0f, 0.0f, 1.0f};
    bool clearDepth = true;
};

class Device {
public:
    virtual ~Device() = default;

    virtual Backend backend() const = 0;
    virtual std::string deviceName() const = 0;
    virtual bool headless() const = 0;
    virtual uint32_t backbufferWidth() const = 0;
    virtual uint32_t backbufferHeight() const = 0;
    virtual Format backbufferFormat() const = 0;
    // True when the presentation target is a Vulkan swapchain, whose row 0 is
    // the top of the screen (so the present pass must flip the image vertically).
    virtual bool presentFlipsY() const = 0;

    virtual std::unique_ptr<Buffer> createBuffer(BufferUsage usage, size_t size) = 0;
    virtual std::unique_ptr<Texture> createTexture(const TextureDesc& desc, const void* rgba8 = nullptr) = 0;
    virtual std::unique_ptr<Pipeline> createPipeline(const PipelineDesc& desc) = 0;
    virtual std::unique_ptr<DescriptorSet> createDescriptorSet(SetType type) = 0;

    // Frame lifecycle. beginFrame returns false when the frame must be skipped
    // (e.g. minimised window); endFrame optionally captures the backbuffer,
    // retrievable with takeCapture() as RGBA8, top row first.
    virtual bool beginFrame() = 0;
    virtual void endFrame(bool capture = false) = 0;
    virtual std::vector<uint8_t> takeCapture() = 0;
    virtual void waitIdle() = 0;
    // Resizes the offscreen backbuffer (headless only). Returns false when the
    // backend cannot resize, in which case the caller keeps its current size.
    // Callers must re-query backbufferWidth/Height and rebuild size-dependent targets.
    virtual bool resizeBackbuffer(uint32_t width, uint32_t height) { (void)width; (void)height; return false; }

    // Commands (recorded between beginFrame/endFrame).
    virtual void beginPass(const PassDesc& pass) = 0;
    virtual void endPass() = 0;
    virtual void setViewport(int x, int y, int w, int h) = 0;
    virtual void bindPipeline(Pipeline* pipeline) = 0;
    virtual void bindDescriptorSet(uint32_t index, DescriptorSet* set) = 0;
    virtual void bindVertexBuffer(Buffer* buffer) = 0;
    virtual void bindIndexBuffer(Buffer* buffer) = 0;
    virtual void pushConstants(const void* data, size_t size) = 0;   // max 128 bytes
    virtual void drawIndexed(uint32_t indexCount, uint32_t firstIndex = 0) = 0;
    // Instanced draw of the bound indexed mesh (step 3 foliage). Instance index comes from the draw, not a vertex buffer.
    virtual void drawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount) = 0;
    virtual void drawFullscreen() = 0;
    // Non-indexed draw of `vertexCount` vertices, `instanceCount` times. Procedural vertex
    // shaders read gl_VertexID / gl_InstanceID (INSTANCE_ID in common.glsl).
    virtual void drawInstanced(uint32_t vertexCount, uint32_t instanceCount) = 0;
};

// Creates a device for the requested backend. `window` may be null for headless
// Vulkan; the GL backend always needs a current context (a hidden window works).
std::unique_ptr<Device> createDevice(Backend backend, const DeviceConfig& config, GLFWwindow* window);

// Reads a whole file; throws on failure.
std::vector<uint8_t> readFileBytes(const std::string& path);

} // namespace rhi
