#include "engine/cull.h"
#include "engine/vfs.h"
#include "engine/renderer.h"

#include <fstream>
#include <iterator>

#include "stb_image.h"

#include "engine/hud_renderer.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>

#include "stb_image.h"
#include "engine/blue_noise_64.h"

namespace ukiyo {
namespace {

// Index buffer and count for a draw's LOD level (0 = full detail). Every pass that draws a Rec uses this, so the
// depth, velocity, normal and colour passes agree on the surface.
void lodIndices(const Primitive& prim, int lod, rhi::Buffer** ib, uint32_t* ic) {
    const size_t li = std::min(static_cast<size_t>(std::max(lod, 0)), prim.lods.size());
    *ib = li > 0 ? prim.lods[li - 1].indices.get() : prim.indices.get();
    *ic = li > 0 ? prim.lods[li - 1].indexCount : prim.indexCount;
}

constexpr uint32_t kShadowAtlas = 4096;
constexpr uint32_t kCascadeSize = kShadowAtlas / 2;
constexpr float kSplits[3] = {8.0f, 25.0f, 80.0f};   // view distances where cascades end

// Fits a light-space orthographic box around the camera sub-frustum [n, f].
// Snaps the box to shadow-texel steps so shadows do not shimmer as the camera moves.
glm::mat4 fitCascade(const OrbitCamera& cam, float aspect, float n, float f, const glm::vec3& L) {
    const glm::mat4 subProj = glm::perspectiveRH_ZO(cam.fovY, aspect, n, f);
    const glm::mat4 invVP = glm::inverse(subProj * cam.view());

    glm::vec3 corners[8];
    int k = 0;
    for (float x : {-1.0f, 1.0f})
        for (float y : {-1.0f, 1.0f})
            for (float z : {0.0f, 1.0f}) {
                glm::vec4 p = invVP * glm::vec4(x, y, z, 1.0f);
                corners[k++] = glm::vec3(p) / p.w;
            }
    glm::vec3 center(0.0f);
    for (const auto& c : corners) center += c;
    center /= 8.0f;
    float radius = 0.0f;
    for (const auto& c : corners) radius = std::max(radius, glm::length(c - center));
    radius = std::ceil(radius * 16.0f) / 16.0f;

    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    glm::mat4 view = glm::lookAtRH(center + L * radius * 2.0f, center, up);
    const float texel = 2.0f * radius / float(kCascadeSize);
    glm::vec3 lc = glm::vec3(view * glm::vec4(center, 1.0f));
    float dx = std::round(lc.x / texel) * texel - lc.x;
    float dy = std::round(lc.y / texel) * texel - lc.y;
    view = glm::translate(glm::mat4(1.0f), glm::vec3(dx, dy, 0.0f)) * view;

    const glm::mat4 proj = glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.01f, radius * 4.0f);
    return proj * view;
}

// A colour as RGBA8 (0..1 per channel), kept in a float's bits so it fits a push-constant vec4 slot.
float packBits(const glm::vec3& rgb) {
    const uint32_t bits = glm::packUnorm4x8(glm::vec4(rgb, 1.0f));
    float out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

PushConstants pcWith(const glm::mat4& model, glm::vec4 p0, glm::vec4 p1 = glm::vec4(0.0f),
                     glm::vec4 p2 = glm::vec4(0.0f), glm::vec4 p3 = glm::vec4(0.0f)) {
    PushConstants pc{};
    pc.model = model;
    pc.p0 = p0;
    pc.p1 = p1;
    pc.p2 = p2;
    pc.p3 = p3;
    return pc;
}

} // namespace

Renderer::Renderer(rhi::Device& device) : dev_(device) {
    frameUbo_ = dev_.createBuffer(rhi::BufferUsage::Uniform, sizeof(FrameUBO));
    contactUbo_ = dev_.createBuffer(rhi::BufferUsage::Uniform, sizeof(glm::vec4));

    rhi::TextureDesc sd;
    sd.width = kShadowAtlas;
    sd.height = kShadowAtlas;
    sd.format = rhi::Format::D32F;
    sd.filter = rhi::Filter::Nearest;
    sd.wrap = rhi::Wrap::Clamp;
    shadowAtlas_ = dev_.createTexture(sd, nullptr);

    rhi::TextureDesc dd;
    dd.width = 1;
    dd.height = 1;
    dd.format = rhi::Format::RGBA8;
    const uint8_t transparent[4] = {0, 0, 0, 0};
    noAlbedo_ = dev_.createTexture(dd, transparent);
    const uint8_t white[4] = {255, 255, 255, 255};
    dummyTex_ = dev_.createTexture(dd, white);

    // The Frame set's binding 8 carries the environment cubemap (docs/IBL.md); a 1x1 white cube stands in until a
    // scene asks for one, so every pipeline's descriptor is always satisfied and sampling it is harmless.
    rhi::TextureDesc cd;
    cd.width = 1;
    cd.height = 1;
    cd.format = rhi::Format::RGBA8;
    cd.faces = 6;
    dummyCube_ = dev_.createTexture(cd, white);

    frameSet_ = dev_.createDescriptorSet(rhi::SetType::Frame);
    frameSet_->setBuffer(0, frameUbo_.get());
    frameSet_->setTexture(1, shadowAtlas_.get());
    frameSet_->setTexture(8, dummyCube_.get());

    // The shadow pass renders into the atlas, so it must not also sample it.
    shadowFrameSet_ = dev_.createDescriptorSet(rhi::SetType::Frame);
    shadowFrameSet_->setBuffer(0, frameUbo_.get());
    shadowFrameSet_->setTexture(1, dummyTex_.get());
    shadowFrameSet_->setTexture(8, dummyCube_.get());

    buildPipelines();
}

void Renderer::setMmdToon(const Primitive* prim, bool on)
{
    if (on)
        mmdToon_.insert(prim);
    else
        mmdToon_.erase(prim);
}

void Renderer::setGraphMaterial(const Primitive* prim, GraphMaterial* material)
{
    if (material)
        graphMaterials_[prim] = material;
    else
        graphMaterials_.erase(prim);
}

Renderer::~Renderer() {
    dev_.waitIdle();
}

void Renderer::buildPipelines() {
    auto make = [&](const char* vs, const char* fs, std::vector<rhi::SetType> sets, bool vertexInput,
                    rhi::Cull cull, bool depthTest, bool depthWrite, rhi::Blend blend,
                    std::vector<rhi::Format> colors, bool hasDepth) {
        rhi::PipelineDesc d;
        d.vertexShader = vs;
        d.fragmentShader = fs;
        d.sets = std::move(sets);
        d.vertexInput = vertexInput;
        d.cull = cull;
        d.depthTest = depthTest;
        d.depthWrite = depthWrite;
        d.blend = blend;
        d.colorFormats = std::move(colors);
        d.hasDepth = hasDepth;
        // A wireframe view turns the geometry passes into their edges and leaves every fullscreen pass alone: a post
        // shader in line mode would draw two long triangles. The shadow map stays solid, so the picture keeps its
        // contact shadows where a surface is shaded at all.
        d.wireframe = wireframe_ && std::string(vs) != "fullscreen" && std::string(vs) != "shadow";
        return dev_.createPipeline(d);
    };
    using S = rhi::SetType;
    using F = rhi::Format;
    const std::vector<S> sceneSets = {S::Frame, S::Material, S::Skin};

    pShadow_ = make("shadow", "shadow", sceneSets, true, rhi::Cull::None, true, true, rhi::Blend::None, {}, true);
    pSky_ = make("fullscreen", "sky", {S::Frame}, false, rhi::Cull::None, false, false, rhi::Blend::None,
                 {F::RGBA16F}, true);
    // The grid writes depth, as before, so depth-based effects see it. In a water scene the grid tests depth but does
    // not write it: the water drawn after it must win where a wave dips below y = 0 (pGridWater_).
    pGrid_ = make("fullscreen", "grid", {S::Frame}, false, rhi::Cull::None, true, true, rhi::Blend::None,
                  {F::RGBA16F}, true);
    pGridWater_ = make("fullscreen", "grid", {S::Frame}, false, rhi::Cull::None, true, false, rhi::Blend::None,
                       {F::RGBA16F}, true);
    for (int blend = 0; blend < 2; ++blend)
        for (int dbl = 0; dbl < 2; ++dbl)
            pMToon_[blend][dbl] = make("mtoon", "mtoon", sceneSets, true,
                                       dbl ? rhi::Cull::None : rhi::Cull::Back,
                                       true, blend == 0, blend ? rhi::Blend::Alpha : rhi::Blend::None,
                                       {F::RGBA16F}, true);
    for (int dbl = 0; dbl < 2; ++dbl)
        pDepth_[dbl] = make("mtoon", "depth", sceneSets, true, dbl ? rhi::Cull::None : rhi::Cull::Back,
                            true, true, rhi::Blend::None, {}, true);
    pOutline_ = make("outline", "outline", sceneSets, true, rhi::Cull::Front, true, true, rhi::Blend::None,
                     {F::RGBA16F}, true);
    for (int blend = 0; blend < 2; ++blend)
        for (int dbl = 0; dbl < 2; ++dbl)
            pMmdToon_[blend][dbl] = make("mtoon", "mmdtoon", sceneSets, true,
                                         dbl ? rhi::Cull::None : rhi::Cull::Back,
                                         true, blend == 0, blend ? rhi::Blend::Alpha : rhi::Blend::None,
                                         {F::RGBA16F}, true);
    pMmdEdge_ = make("mmdedge", "mmdedge", sceneSets, true, rhi::Cull::Front, true, true, rhi::Blend::None,
                     {F::RGBA16F}, true);

    pBloomDown_ = make("fullscreen", "bloom_down", {S::Post}, false, rhi::Cull::None, false, false,
                       rhi::Blend::None, {F::RGBA16F}, false);
    pBloomBlur_ = make("fullscreen", "bloom_blur", {S::Post}, false, rhi::Cull::None, false, false,
                       rhi::Blend::None, {F::RGBA16F}, false);
    pBloomCombine_ = make("fullscreen", "bloom_combine", {S::Post}, false, rhi::Cull::None, false, false,
                          rhi::Blend::None, {F::RGBA16F}, false);
    pTonemap_ = make("fullscreen", "tonemap", {S::Post}, false, rhi::Cull::None, false, false,
                     rhi::Blend::None, {F::RGBA8}, false);
    pFxaa_ = make("fullscreen", "fxaa", {S::Post}, false, rhi::Cull::None, false, false,
                  rhi::Blend::None, {F::RGBA8}, false);
    pStylize_ = make("fullscreen", "stylize", {S::Post}, false, rhi::Cull::None, false, false,
                     rhi::Blend::None, {F::RGBA8}, false);
    pTaa_ = make("fullscreen", "taa", {S::Post}, false, rhi::Cull::None, false, false,
                 rhi::Blend::None, {F::RGBA16F}, false);
    pVelocity_ = make("velocity", "velocity", {S::Frame, S::Material, S::Skin, S::Motion}, true,
                      rhi::Cull::Back, true, false, rhi::Blend::None, {F::RGBA16F}, true);
    // The normal pass writes two targets: view-space normals, and the base colour the multi-bounce AO fit needs.
    pNormal_ = make("normal", "normal", {S::Frame, S::Material, S::Skin}, true,
                    rhi::Cull::Back, true, false, rhi::Blend::None, {F::RGBA8, F::RGBA8}, true);
    pEffects_ = make("fullscreen", "effects", {S::Post}, false, rhi::Cull::None, false, false,
                     rhi::Blend::None, {F::RGBA16F}, false);
    // Real-time GI: the trace pass needs the Frame set (camera basis, ambient colours, projection) *and* the Post set
    // (heightmap, raster, depth, normals). The Frame set must be set 0 and the Post set 1, because the shader includes
    // frame.glsl, whose uniforms live in set 0 on Vulkan; the renderer binds the sets in this order.
    pGiTrace_ = make("fullscreen", "gi_trace", {S::Frame, S::Post}, false, rhi::Cull::None, false, false,
                     rhi::Blend::None, {F::RGBA16F}, false);
    pGiAccum_ = make("fullscreen", "gi_accum", {S::Post}, false, rhi::Cull::None, false, false,
                     rhi::Blend::None, {F::RGBA16F}, false);
    // The denoiser: two à-trous iterations at half resolution. It is the iGPU answer to the reference's DLSS Ray
    // Reconstruction — an edge-aware filter in place of a network, so it needs no tensor cores.
    pGiDenoise_ = make("fullscreen", "gi_denoise", {S::Post}, false, rhi::Cull::None, false, false,
                       rhi::Blend::None, {F::RGBA16F}, false);
    pExpo_ = make("fullscreen", "expo_reduce", {S::Post}, false, rhi::Cull::None, false, false,
                  rhi::Blend::None, {F::RGBA8}, false);
    pPresent_ = make("fullscreen", "present", {S::Post}, false, rhi::Cull::None, false, false,
                     rhi::Blend::None, {F::Backbuffer}, false);
    pWaterCopy_ = make("fullscreen", "water_copy", {S::Post}, false, rhi::Cull::None, false, false,
                       rhi::Blend::None, {F::RGBA16F}, false);
    pWater_ = make("water", "water", {S::Frame, S::Post}, true, rhi::Cull::None, true, false,
                   rhi::Blend::None, {F::RGBA16F}, true);
    pTerrain_ = make("terrain", "terrain", {S::Frame, S::ShaderMaterial}, true, rhi::Cull::None, true, true, rhi::Blend::None,
                     {F::RGBA16F}, true);
    pFoliage_ = make("foliage", "foliage", {S::Frame, S::ShaderMaterial}, true, rhi::Cull::None, true, true,
                     rhi::Blend::None, {F::RGBA16F}, true);
    pGrass_ = make("grass", "grass", {S::Frame, S::ShaderMaterial}, true, rhi::Cull::None, true, true,
                   rhi::Blend::None, {F::RGBA16F}, true);
    pTerrainShadow_ = make("terrain_shadow", "shadow_empty", {S::Frame}, true, rhi::Cull::None, true, true,
                           rhi::Blend::None, {}, true);
    pUnderwater_ = make("fullscreen", "underwater", {S::Post}, false, rhi::Cull::None, false, false,
                        rhi::Blend::Alpha, {F::RGBA16F}, false);

    // The virtual-geometry cut runs on the compute queue when its shader is built (docs/VGEOM.md §7). A missing
    // or failing vgeom_cut.comp is not an error — a tool or test harness may run without the shaderc output — so
    // the failure keeps the CPU walk: vgGpu_ only picks between two paths that select the same clusters.
    if (!pVGeomCut_) {
        try {
            rhi::PipelineDesc c;
            c.computeShader = "vgeom_cut";
            c.sets = {rhi::SetType::Compute};
            pVGeomCut_ = dev_.createPipeline(c);
            vgGpu_ = true;
        } catch (...) {
            pVGeomCut_.reset();
            vgGpu_ = false;
        }
    }
}

void Renderer::setSceneLights(std::vector<SceneLight> lights) {
    sceneLights_ = std::move(lights);
}

// The environment bake (docs/IBL.md). Everything happens here, once per scene load: the cubemap from the procedural
// sky or a panorama PNG, the prefiltered mip chain for the reflection, the probe volumes with the scene's lights
// baked in, and the whole-scene fallback probe. Per frame, render() only interpolates the volumes and packs.
bool Renderer::setEnvironment(const RenderSettings& settings, std::string* error) {
    envIntensity_ = 0.0f;
    envVolumes_.clear();
    envWholeScene_ = env::Sh::zero();
    const bool hadCube = envCube_ != nullptr;
    envCube_.reset();
    frameSet_->setTexture(8, dummyCube_.get());
    shadowFrameSet_->setTexture(8, dummyCube_.get());

    const env::Settings& e = settings.environment;
    (void)hadCube;   // the binding above already reverted any previous environment
    if (!e.enabled) return true;

    // The key light the sky is baked with: the same pick render() makes, so the environment and the direct sun agree.
    glm::vec3 sunDir{0.5f, 0.6f, 0.4f};
    for (const SceneLight& sl : sceneLights_) {
        if (!sl.enabled || sl.type != LightType::Directional) continue;
        const float len = glm::length(sl.direction);
        if (len < 1e-5f) continue;
        sunDir = -sl.direction / len;
        break;
    }

    env::Cubemap cube;
    if (e.fromSky || e.panorama.empty()) {
        cube = env::bakeSkyCubemap(settings.skyZenith, settings.skyHorizon, sunDir,
                                   std::clamp(settings.sunDisc, 0.0f, 1.0f),
                                   std::clamp(settings.cloudCover, 0.0f, 1.0f), e.cubeSize);
    } else {
        std::ifstream in(e.panorama, std::ios::binary);
        if (!in) {
            if (error) *error = "environment panorama could not be opened: " + e.panorama;
            return false;
        }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        int w = 0, h = 0, comp = 0;
        stbi_uc* px = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp, 4);
        if (!px) {
            if (error) *error = "environment panorama is not a readable image: " + e.panorama;
            return false;
        }
        cube = env::fromEquirect(px, w, h, e.cubeSize);
        stbi_image_free(px);
    }

    // The reflection chain: level 0 plus one prefiltered GGX level per mip, packed face-major RGBA8.
    const std::vector<env::Cubemap> chain = env::prefilteredChain(cube);
    const std::vector<std::vector<uint8_t>> mipBytes = env::packMipChain(cube, chain);
    rhi::TextureDesc td;
    td.width = static_cast<uint32_t>(cube.size);
    td.height = static_cast<uint32_t>(cube.size);
    td.format = rhi::Format::RGBA8;
    td.mipmaps = true;
    td.filter = rhi::Filter::Linear;
    td.wrap = rhi::Wrap::Clamp;
    td.faces = 6;
    envCube_ = dev_.createTexture(td, nullptr, &mipBytes);
    envMips_ = static_cast<int>(mipBytes.size());
    frameSet_->setTexture(8, envCube_.get());
    shadowFrameSet_->setTexture(8, envCube_.get());

    // The lights, in bake terms. The sun is one of them: a probe in the open must shade brighter towards it, the
    // same way the direct light does.
    std::vector<env::EnvLight> lights;
    for (const SceneLight& sl : sceneLights_) {
        if (!sl.enabled) continue;
        env::EnvLight el;
        el.type = static_cast<int>(sl.type);
        el.position = sl.position;
        el.direction = sl.direction;
        el.color = sl.color;
        el.intensity = sl.intensity;
        el.radius = sl.radius;
        el.spotAngleDeg = sl.spotAngleDeg;
        el.spotSoftness = sl.spotSoftness;
        lights.push_back(el);
    }

    envWholeScene_ = env::irradianceFromCubemap(cube);
    for (const env::EnvLight& el : lights) addLightToSh(&envWholeScene_, glm::vec3(0.0f), el);
    for (const auto& vd : e.volumes) {
        env::ProbeVolume pv;
        pv.center = vd.center;
        pv.halfExtent = glm::max(vd.size * 0.5f, glm::vec3(0.05f));
        pv.divX = std::max(1, vd.divX);
        pv.divY = std::max(1, vd.divY);
        pv.divZ = std::max(1, vd.divZ);
        env::bakeVolume(&pv, env::irradianceFromCubemap(cube), lights);
        envVolumes_.push_back(std::move(pv));
    }

    envIntensity_ = std::max(e.intensity, 0.0f);
    return true;
}

void Renderer::setWater(const WaterVolume* volume) {
    if (!volume) {
        water_.reset();
        return;
    }
    water_ = *volume;
    if (!waterVb_) {
        // Rest grid over the unit square. Each vertex carries its (u, v) in pos.xz and uv.
        constexpr uint32_t kN = 128;
        std::vector<rhi::Vertex> verts((kN + 1) * (kN + 1));
        for (uint32_t j = 0; j <= kN; ++j)
            for (uint32_t i = 0; i <= kN; ++i) {
                rhi::Vertex& v = verts[j * (kN + 1) + i];
                const float u = float(i) / float(kN);
                const float w = float(j) / float(kN);
                v.pos = glm::vec3(u, 0.0f, w);
                v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
                v.uv = glm::vec2(u, w);
                v.weights = glm::vec4(0.0f);
            }
        std::vector<uint32_t> idx;
        idx.reserve(kN * kN * 6);
        for (uint32_t j = 0; j < kN; ++j)
            for (uint32_t i = 0; i < kN; ++i) {
                const uint32_t a = j * (kN + 1) + i;
                const uint32_t b = a + 1;
                const uint32_t c = a + (kN + 1);
                const uint32_t d = c + 1;
                idx.insert(idx.end(), {a, c, b, b, c, d});
            }
        waterIndexCount_ = static_cast<uint32_t>(idx.size());
        waterVb_ = dev_.createBuffer(rhi::BufferUsage::Vertex, verts.size() * sizeof(rhi::Vertex));
        waterVb_->upload(verts.data(), verts.size() * sizeof(rhi::Vertex));
        waterIb_ = dev_.createBuffer(rhi::BufferUsage::Index, idx.size() * sizeof(uint32_t));
        waterIb_->upload(idx.data(), idx.size() * sizeof(uint32_t));
    }
}

void Renderer::rebuildTargets(uint32_t scale) {
    dev_.waitIdle();
    displayW_ = dev_.backbufferWidth();
    displayH_ = dev_.backbufferHeight();
    scale_ = scale;
    intW_ = std::max(1u, displayW_ * scale);
    intH_ = std::max(1u, displayH_ * scale);

    auto color = [&](uint32_t w, uint32_t h, rhi::Format f) {
        rhi::TextureDesc d;
        d.width = std::max(1u, w);
        d.height = std::max(1u, h);
        d.format = f;
        d.filter = rhi::Filter::Linear;
        d.wrap = rhi::Wrap::Clamp;
        return dev_.createTexture(d, nullptr);
    };

    hdr_ = color(intW_, intH_, rhi::Format::RGBA16F);
    ldr0_ = color(intW_, intH_, rhi::Format::RGBA8);
    ldr1_ = color(intW_, intH_, rhi::Format::RGBA8);
    {
        rhi::TextureDesc d;
        d.width = intW_;
        d.height = intH_;
        d.format = rhi::Format::D32F;
        d.filter = rhi::Filter::Nearest;
        depth_ = dev_.createTexture(d, nullptr);
    }
    // Bloom level 0 is bloomScale_ of the internal frame (0.5 = half, the original size); each level halves.
    const uint32_t bloomW = std::max(1u, static_cast<uint32_t>(intW_ * bloomScale_));
    const uint32_t bloomH = std::max(1u, static_cast<uint32_t>(intH_ * bloomScale_));
    for (uint32_t i = 0; i < bloom_.size(); ++i)
        bloom_[i] = color(std::max(1u, bloomW >> i), std::max(1u, bloomH >> i), rhi::Format::RGBA16F);
    for (uint32_t i = 0; i < bloomTmp_.size(); ++i)
        bloomTmp_[i] = color(std::max(1u, bloomW >> i), std::max(1u, bloomH >> i), rhi::Format::RGBA16F);
    bloomOut_ = color(bloomW, bloomH, rhi::Format::RGBA16F);
    for (auto& h : taaHist_) h = color(intW_, intH_, rhi::Format::RGBA16F);
    velocity_ = color(intW_, intH_, rhi::Format::RGBA16F);
    refr_ = color(std::max(1u, intW_ / 2), std::max(1u, intH_ / 2), rhi::Format::RGBA16F);
    normal_ = color(intW_, intH_, rhi::Format::RGBA8);
    albedo_ = color(intW_, intH_, rhi::Format::RGBA8);
    fxHdr_ = color(intW_, intH_, rhi::Format::RGBA16F);
    // GI targets at half resolution. The trace reads the previous frame's raster, the depth and the normals, and the
    // heightmap at binding 5; the accumulator reads the trace and its own history; the composite is folded into the
    // effects pass, which reads the accumulator at binding 5 on a frame where GI is on.
    gi_ = color(std::max(1u, intW_ / 2), std::max(1u, intH_ / 2), rhi::Format::RGBA16F);
    giDenoise_ = color(std::max(1u, intW_ / 2), std::max(1u, intH_ / 2), rhi::Format::RGBA16F);
    for (auto& h : giHist_) h = color(std::max(1u, intW_ / 2), std::max(1u, intH_ / 2), rhi::Format::RGBA16F);
    for (auto& e : expo_) {   // 1x1 state, starts at 0.5 = exposure multiplier 1
        rhi::TextureDesc d;
        d.width = 1;
        d.height = 1;
        d.format = rhi::Format::RGBA8;
        d.filter = rhi::Filter::Nearest;
        d.wrap = rhi::Wrap::Clamp;
        const uint8_t mid[4] = {128, 128, 128, 255};
        e = dev_.createTexture(d, mid);
    }
    taaHistoryValid_ = false;     // a resize invalidates the history
    taaWrite_ = 0;

    buildPostSets();
}

namespace {

// Identity grading strip: texel (x, y) holds the colour (r, g, b) = (x % 16, y, x / 16) / 15.
std::vector<uint8_t> identityLutPixels() {
    std::vector<uint8_t> px(size_t(256) * 16 * 4);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 256; ++x) {
            const int r = x % 16, b = x / 16;
            uint8_t* p = &px[(size_t(y) * 256 + size_t(x)) * 4];
            p[0] = uint8_t(std::lround(r * 255.0 / 15.0));
            p[1] = uint8_t(std::lround(y * 255.0 / 15.0));
            p[2] = uint8_t(std::lround(b * 255.0 / 15.0));
            p[3] = 255;
        }
    return px;
}

} // namespace

namespace {

// Reads a .cube 3D LUT and unrolls it into the strip the grading shader samples: size*size x size texels,
// texel (x = b*size + r, y = g) holds the colour at .cube index r + g*size + b*size*size.
bool readCubeStrip(const std::string& path, std::vector<uint8_t>& px, uint32_t& size, std::string* error) {
    std::ifstream in(path);
    if (!in) {
        if (error) *error = "cannot read LUT " + path;
        return false;
    }
    uint32_t n = 0;
    std::vector<float> vals;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string tok;
        if (!(ss >> tok) || tok[0] == '#') continue;
        if (tok == "LUT_3D_SIZE") {
            ss >> n;
            continue;
        }
        if (tok == "LUT_1D_SIZE") {
            if (error) *error = "1D .cube LUTs are not supported: " + path;
            return false;
        }
        if (tok == "TITLE" || tok == "DOMAIN_MIN" || tok == "DOMAIN_MAX") continue;
        std::istringstream ds(line);
        float r, g, b;
        if (ds >> r >> g >> b) {
            vals.push_back(r);
            vals.push_back(g);
            vals.push_back(b);
        }
    }
    if (n < 2 || n > 128 || vals.size() != size_t(n) * n * n * 3) {
        if (error) *error = "LUT_3D_SIZE does not match the data count: " + path;
        return false;
    }
    size = n;
    const uint32_t w = n * n;
    px.assign(size_t(w) * n * 4, 255);
    for (uint32_t b = 0; b < n; ++b)
        for (uint32_t g = 0; g < n; ++g)
            for (uint32_t r = 0; r < n; ++r) {
                const size_t idx = (size_t(r) + size_t(g) * n + size_t(b) * n * n) * 3;
                uint8_t* p = &px[(size_t(g) * w + (size_t(b) * n + r)) * 4];
                for (int c = 0; c < 3; ++c)
                    p[c] = uint8_t(std::lround(std::clamp(vals[idx + c], 0.0f, 1.0f) * 255.0f));
            }
    return true;
}

} // namespace

std::unique_ptr<rhi::Texture> Renderer::makeLutTexture(const uint8_t* rgba, uint32_t size) const {
    rhi::TextureDesc d;
    d.width = size * size;
    d.height = size;
    d.format = rhi::Format::RGBA8;
    d.filter = rhi::Filter::Linear;
    d.wrap = rhi::Wrap::Clamp;
    return dev_.createTexture(d, rgba);
}

bool Renderer::setGradingLut(const std::string& pngPath, std::string* error) {
    if (pngPath.empty()) {
        const std::vector<uint8_t> id = identityLutPixels();
        dev_.waitIdle();
        lut_ = makeLutTexture(id.data());
        hasLut_ = false;
        if (hdr_) buildPostSets();   // targets are built on the first frame; they bind lut_ then
        return true;
    }
    if (pngPath.size() >= 5 && pngPath.compare(pngPath.size() - 5, 5, ".cube") == 0) {
        std::vector<uint8_t> strip;
        uint32_t n = 0;
        if (!readCubeStrip(pngPath, strip, n, error)) return false;
        dev_.waitIdle();
        lut_ = makeLutTexture(strip.data(), n);
        hasLut_ = true;
        if (hdr_) buildPostSets();
        return true;
    }
    int w = 0, h = 0, c = 0;
    std::vector<uint8_t> lutBytes;
    stbi_uc* px = vfs::readBytes(pngPath, lutBytes) && !lutBytes.empty()
        ? stbi_load_from_memory(lutBytes.data(), static_cast<int>(lutBytes.size()), &w, &h, &c, 4)
        : nullptr;
    if (!px) {
        if (error) *error = "cannot read LUT " + pngPath;
        return false;
    }
    if (w != 256 || h != 16) {
        stbi_image_free(px);
        if (error) *error = "LUT must be 256x16 (16 slices of 16x16): " + pngPath;
        return false;
    }
    dev_.waitIdle();
    lut_ = makeLutTexture(px);
    stbi_image_free(px);
    hasLut_ = true;
    if (hdr_) buildPostSets();
    return true;
}

void Renderer::buildPostSets() {
    // The identity strip is created on first use so that every post set can bind it.
    if (!lut_) {
        const std::vector<uint8_t> id = identityLutPixels();
        lut_ = makeLutTexture(id.data());
    }
    // The blue-noise tile is expanded to RGBA8 once: the value is in R, G and B, and alpha is 1.
    if (!blueNoise_) {
        std::vector<uint8_t> px(size_t(kBlueNoiseSize) * kBlueNoiseSize * 4);
        for (size_t i = 0; i < size_t(kBlueNoiseSize) * kBlueNoiseSize; ++i) {
            px[i * 4 + 0] = px[i * 4 + 1] = px[i * 4 + 2] = kBlueNoise64[i];
            px[i * 4 + 3] = 255;
        }
        rhi::TextureDesc d;
        d.width = kBlueNoiseSize;
        d.height = kBlueNoiseSize;
        d.format = rhi::Format::RGBA8;
        d.filter = rhi::Filter::Nearest;
        d.wrap = rhi::Wrap::Repeat;
        blueNoise_ = dev_.createTexture(d, px.data());
    }
    auto make = [&](rhi::Texture* a, rhi::Texture* b = nullptr) {
        auto s = dev_.createDescriptorSet(rhi::SetType::Post);
        s->setTexture(1, a);
        if (b) s->setTexture(2, b);
        s->setTexture(3, lut_.get());
        s->setTexture(4, depth_.get());
        s->setTexture(6, blueNoise_.get());
        return s;
    };
    // Water: the copy pass reads hdr_ and depth_ (not attached); the water pass reads refr_ only.
    waterCopySet_ = make(hdr_.get());
    waterSet_ = dev_.createDescriptorSet(rhi::SetType::Post);
    waterSet_->setTexture(2, refr_.get());
    // Post-process chain: effects read hdr_ and write fxHdr_; TAA, bloom and tonemap read fxHdr_.
    setDown_[0] = make(fxHdr_.get());
    setBlurH_[0] = make(bloom_[0].get());
    for (int i = 1; i < 4; ++i) setBlurH_[i] = make(bloom_[i - 1].get());
    for (int i = 0; i < 4; ++i) setBlurV_[i] = make(bloomTmp_[i].get());
    setCombine_ = dev_.createDescriptorSet(rhi::SetType::Post);
    for (uint32_t i = 0; i < 4; ++i) setCombine_->setTexture(i + 1, bloom_[i].get());
    setCombine_->setTexture(6, blueNoise_.get());
    setTonemap_ = make(fxHdr_.get(), bloomOut_.get());
    setTonemap_->setTexture(5, expo_[0].get());
    setFxaa_ = make(ldr0_.get());
    setFxaaLdr1_ = make(ldr1_.get());
    setStylize_ = make(ldr0_.get());
    setPresent_ = make(ldr0_.get());
    setPresentFx_ = make(ldr1_.get());
    for (int k = 0; k < 2; ++k) {
        setTaa_[k] = make(fxHdr_.get(), taaHist_[1 - k].get());        // reads the other history
        setDownTaa_[k] = make(taaHist_[k].get());                      // resolved frame k feeds bloom
        setTonemapTaa_[k] = make(taaHist_[k].get(), bloomOut_.get());  // and tonemap
        setTonemapTaa_[k]->setTexture(5, expo_[0].get());
        setTaa_[k]->setTexture(5, velocity_.get());                    // motion vectors
    }
    setEffects_ = dev_.createDescriptorSet(rhi::SetType::Post);
    setEffects_->setTexture(1, hdr_.get());
    setEffects_->setTexture(2, normal_.get());
    setEffects_->setTexture(3, velocity_.get());
    setEffects_->setTexture(4, depth_.get());
    // GI is composited at binding 5. The effects pass binds it every frame it runs; when GI is off the slot still has
    // to hold a valid image, because a sampled binding cannot be left empty.
    setEffects_->setTexture(5, blueNoise_.get());
    // Binding 6 is the albedo for the multi-bounce AO fit. It is rebound per frame to either the G-buffer target or a
    // 1x1 transparent texture, which is how the shader is told the approximation is off (alpha 0 = no albedo).
    setEffects_->setTexture(6, noAlbedo_.get());
    setEffects_->setBuffer(7, contactUbo_.get());
    setGiTrace_ = dev_.createDescriptorSet(rhi::SetType::Post);
    // Unit 1 is the Frame set's shadow atlas on the shared GL texture-unit space; 2 is the heightmap (bound per frame,
    // because the terrain under the camera can change), 3 the raster, 4 depth, 5 normals.
    setGiTrace_->setTexture(3, hdr_.get());
    setGiTrace_->setTexture(4, depth_.get());
    setGiTrace_->setTexture(5, normal_.get());
    for (int i = 0; i < 2; ++i) {
        setGiAccum_[i] = dev_.createDescriptorSet(rhi::SetType::Post);
        setGiAccum_[i]->setTexture(1, gi_.get());
        setGiAccum_[i]->setTexture(2, giHist_[1 - i].get());
        setGiAccum_[i]->setTexture(4, depth_.get());
        setGiAccum_[i]->setTexture(5, velocity_.get());   // reprojects the history with the engine's motion vectors
        setGiDenoise_[i] = dev_.createDescriptorSet(rhi::SetType::Post);
        setGiDenoise_[i]->setTexture(1, giHist_[i].get());
        setGiDenoise_[i]->setTexture(4, depth_.get());
        setGiDenoise_[i]->setTexture(5, normal_.get());
    }
    {
        setGiDenoiseB_ = dev_.createDescriptorSet(rhi::SetType::Post);
        setGiDenoiseB_->setTexture(1, giDenoise_.get());
        setGiDenoiseB_->setTexture(4, depth_.get());
        setGiDenoiseB_->setTexture(5, normal_.get());
    }
    giFrame_ = 0;
    giWrite_ = 0;

    setExpoReduce_ = make(fxHdr_.get(), expo_[1].get());   // reads the previous state in tex 2
    setExpoCopy_ = make(expo_[0].get());
}

namespace {

// Halton(2,3) sub-pixel jitter in pixels, -0.5 .. 0.5, cycling over 8 frames.
glm::vec2 haltonJitter(uint32_t frame) {
    auto halton = [](uint32_t i, uint32_t b) {
        float f = 1.0f, r = 0.0f;
        while (i > 0) {
            f /= float(b);
            r += f * float(i % b);
            i /= b;
        }
        return r;
    };
    const uint32_t i = frame % 8 + 1;
    return glm::vec2(halton(i, 2) - 0.5f, halton(i, 3) - 0.5f);
}

} // namespace

void Renderer::setTrails(const std::vector<const Trail*>& alpha, const std::vector<const Trail*>& additive)
{
    trailAlpha_ = alpha;
    trailAdditive_ = additive;
}

namespace {

// The storage-buffer form of VGeomView for the compute cut: five normalized frustum planes (Gribb-Hartmann, the
// same rows and normalization selectVGeom uses), then the eye, the pixel scale, the error budget, the cull flag.
// std430 — five vec4s, one vec4 of eye+scale, four floats — 112 bytes, matching shaders/vgeom_cut.comp.
struct VgGpuView {
    glm::vec4 planes[5];
    glm::vec4 eyePixel;   // eye.xyz, pixelScale in w
    float errorPixels = 1.0f;
    float cullOn = 1.0f;
    float pad0 = 0.0f, pad1 = 0.0f;
};
static_assert(sizeof(VgGpuView) == 112, "VgGpuView must match the cut shader's std430 block");

// The push block for one cut item: the instance's world matrix and its work-buffer region. A mat4 followed by
// four consecutive uints lands at the same offsets (64..76) under both the scalar and std140 layouts, so one
// definition serves both backends' PUSH macros. 80 bytes, inside the 128-byte push limit.
struct VgPush {
    glm::mat4 model{1.0f};
    uint32_t idBase = 0, cmdBase = 0, pow2 = 0, root = 0;
};
static_assert(sizeof(VgPush) == 80, "VgPush must match the cut shader's push block");

void fillVgView(const VGeomView& view, float errorPixels, VgGpuView* out) {
    const glm::mat4& m = view.viewProj;
    auto row = [&m](int i) { return glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]); };
    const glm::vec4 r3 = row(3);
    glm::vec4 planes[5] = {r3 + row(0), r3 - row(0), r3 + row(1), r3 - row(1), r3 - row(2)};
    for (glm::vec4& p : planes) p /= std::max(glm::length(glm::vec3(p)), 1e-6f);
    for (int i = 0; i < 5; ++i) out->planes[i] = planes[i];
    out->eyePixel = glm::vec4(view.eye, view.pixelScale);
    out->errorPixels = errorPixels;
    out->cullOn = view.cull ? 1.0f : 0.0f;
}

uint32_t ceilPow2(uint32_t v) {
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

}  // namespace

void Renderer::render(const OrbitCamera& cam, const std::vector<Model*>& models,
                      float time, uint32_t frameIndex, const RenderSettings& s) {
    drawCalls_ = 0;
    triangles_ = 0;
    trailTriangles_ = 0;
    vgeomStats_ = VGeomStats{};
    // The wireframe view is pipeline state, so the pipelines are built once per change rather than once per frame.
    if (wireframe_ != s.wireframe) {
        wireframe_ = s.wireframe;
        buildPipelines();
    }
    // Smear input: each model's hips displacement since the last render call (world units). The hips
    // carry the animated motion; the root only moves when the app moves the model.
    motionDisp_.clear();
    for (Model* m : models) {
        if (!m) continue;
        glm::vec3 p = glm::vec3(m->root[3]);
        auto h = m->humanoid.find("hips");
        if (h != m->humanoid.end() && h->second >= 0 && h->second < static_cast<int>(m->nodes.size()))
            p = glm::vec3(m->nodes[static_cast<size_t>(h->second)].world[3]);
        auto prev = hipsPrev_.find(m);
        motionDisp_[m] = prev == hipsPrev_.end() ? glm::vec3(0.0f) : p - prev->second;
        hipsPrev_[m] = p;
    }
    const uint32_t scale = (s.aaMode == 2 || s.aaMode == 3) ? 2u : 1u;
    const float bloomScale = glm::clamp(s.bloomScale, 0.1f, 1.0f);
    if (scale != scale_ || bloomScale != bloomScale_ || dev_.backbufferWidth() != displayW_ || dev_.backbufferHeight() != displayH_) {
        bloomScale_ = bloomScale;
        rebuildTargets(scale);
    }

    const float aspect = float(intW_) / float(intH_);
    const glm::vec3 L = glm::normalize(s.sunDir);
    const bool taaOn = (s.aaMode == 4);
    const bool taaReset = taaOn && !taaHistoryValid_;

    // Frame globals. With TAA the projection is offset by a sub-pixel Halton sample each frame.
    FrameUBO f{};
    f.view = cam.view();
    const glm::mat4 projUnjittered = cam.proj(aspect);
    f.proj = projUnjittered;
    if (taaOn) {
        const glm::vec2 j = haltonJitter(frameIndex);   // pixels, -0.5 .. 0.5
        f.proj[2][0] -= 2.0f * j.x / float(intW_);      // RH perspective: NDC shift is -P20
        f.proj[2][1] -= 2.0f * j.y / float(intH_);
    }
    f.viewProj = f.proj * f.view;
    f.invViewProj = glm::inverse(f.viewProj);
    const glm::mat4 viewProjUnjittered = projUnjittered * f.view;
    glm::vec3 eye = cam.eye();
    f.camPos = glm::vec4(eye, 1.0f);
    // Selection view for virtual geometry (item 21). pixelScale is world metres at one metre of depth in pixels, which
    // for a perspective projection is height * P11 / 2; with it, a cluster's error converts straight to pixels.
    VGeomView vgView;
    vgView.viewProj = viewProjUnjittered;
    vgView.eye = eye;
    vgView.pixelScale = 0.5f * float(intH_) * projUnjittered[1][1];
    // The key light. A scene that authors a directional light turns it into the sun: its direction, colour and
    // intensity replace the built-in one, so a key light in the Inspector is the light that casts the cascades.
    glm::vec3 sunDir = L;
    glm::vec3 sunColor = glm::vec3(1.0f, 0.95f, 0.88f) * s.sunIntensity;
    lastKeyFromScene_ = false;
    for (const SceneLight& sl : sceneLights_) {
        if (!sl.enabled || sl.type != LightType::Directional) continue;
        const float len = glm::length(sl.direction);
        if (len < 1e-5f) continue;
        sunDir = -sl.direction / len;   // authored as the direction it shines, used as the direction towards it
        sunColor = sl.color * sl.intensity;
        lastKeyFromScene_ = true;
        break;
    }
    const glm::vec3 Lsun = sunDir;
    f.sunDir = glm::vec4(Lsun, 0.0f);
    f.sunColor = glm::vec4(sunColor, 1.0f);
    // The scene's sky and ambient. These were two hardcoded constants until a game needed a night: the sky shader reads
    // the two colours, and every shaded surface reads the hemisphere below. The 0.9 on the sky ambient is the old
    // constant's own scale, kept so the daytime defaults render unchanged.
    f.skyColor = glm::vec4(s.ambientSky, 1.0f) * 0.9f;
    f.groundColor = glm::vec4(s.ambientGround, 1.0f);
    f.skyZenith = glm::vec4(s.skyZenith, std::clamp(s.cloudCover, 0.0f, 1.0f));
    f.skyHorizon = glm::vec4(s.skyHorizon, std::clamp(s.stars, 0.0f, 1.0f));
    f.skyParams = glm::vec4(std::clamp(s.sunDisc, 0.0f, 1.0f), 0.0f, 0.0f, 0.0f);
    // The environment probe (docs/IBL.md), interpolated at the camera's focus — the same pick the light ranking
    // makes. envParams.x = 0 leaves the hemisphere ambient path in the shader untouched.
    if (envIntensity_ > 0.0f) {
        const env::FrameEnv fe = env::packFrameEnv(envVolumes_, envWholeScene_, cam.target, envIntensity_,
                                                   envCube_ != nullptr, envMips_);
        for (int i = 0; i < 9; ++i) f.envSH[i] = fe.sh[i];
        f.envParams = fe.params;
    } else {
        f.envParams = glm::vec4(0.0f);
    }

    // Cascade ranges: [near, 8], [8, 25], [25, 80].
    const float ranges[4] = {cam.nearZ, kSplits[0], kSplits[1], kSplits[2]};
    for (int c = 0; c < 3; ++c) f.shadowMatrix[c] = fitCascade(cam, aspect, ranges[c], ranges[c + 1], Lsun);
    f.cascadeSplits = glm::vec4(kSplits[0], kSplits[1], kSplits[2], 0.0f);
    f.shadowParams = glm::vec4(s.shadows ? 1.0f : 0.0f, 1.0f / float(kShadowAtlas), 0.02f, 0.0008f);
    // Percentage-closer soft shadows ride in their own vec4: the sun's angular radius, the switch, the search radius and
    // the cap. Left zero when the feature is off, which is the shader's test (docs/PCSS.md).
    f.shadowSoft = (s.shadows && s.pcss)
                        ? glm::vec4(std::max(s.pcssSunTanRadius, 1e-4f), 1.0f, std::max(s.pcssSearchRadius, 1e-3f),
                                    std::max(s.pcssMaxPenumbra, 1e-3f))
                        : glm::vec4(0.0f);
    f.screenParams = glm::vec4(float(intW_), float(intH_), time, float(frameIndex));

    // Scene lights: rank the point and spot lights against the camera's focus and pack the best ones. A camera looking
    // at a lantern gets the lantern, not the two lights furthest away. Directional lights were taken above.
    {
        std::vector<SceneLight> local;
        local.reserve(sceneLights_.size());
        for (const SceneLight& sl : sceneLights_)
            if (sl.type != LightType::Directional) local.push_back(sl);
        const glm::vec3 focus = cam.target;
        const glm::vec3 forward = cam.eye() - cam.target;   // the direction the camera looks
        lastSceneLightCount_ = packSceneLights(local, focus, glm::length(forward) > 1e-5f ? glm::normalize(forward)
                                                                                         : glm::vec3(0, 0, -1),
                                               kFrameLights, f.lights);
        f.lightParams = glm::vec4(float(lastSceneLightCount_), lastKeyFromScene_ ? 1.0f : 0.0f, 0.0f, 0.0f);
    }
    // Two soft coloured point lights for the anime rim/fill look, used only by a scene that authors no lights.
    f.pointPos[0] = glm::vec4(1.4f, 1.6f, 1.2f, 3.0f);
    f.toonA = glm::vec4(s.hairHighlight, s.hairShift, s.hairExponent, s.selfShadow ? 1.0f : 0.0f);
    f.toonB = glm::vec4(s.selfThreshold, s.selfHardness, s.gloss, s.glossPower);
    // A scene with an environment may override the reflection strength without touching the project default
    // (environment.reflectionStrength < 0 keeps `reflection`).
    const float reflectionStrength = s.environment.reflectionStrength >= 0.0f ? s.environment.reflectionStrength
                                                                              : s.reflection;
    f.toonC = glm::vec4(s.celBand ? 1.0f : 0.0f, s.normalMap, s.outlineMatNoise, reflectionStrength);
    if (s.faceShadow && !models.empty() && models[0]) {
        // Head forward: the model's facing turned by its root transform, flattened to the ground plane.
        glm::vec3 fw = glm::vec3(models[0]->root * glm::vec4(models[0]->facing, 0.0f));
        fw.y = 0.0f;
        if (glm::length(fw) > 1e-5f) fw = glm::normalize(fw);
        f.toonD = glm::vec4(fw.x, fw.z, 1.0f, 0.0f);
    }
    f.toonD.w = s.smear;
    f.toonE = glm::vec4(s.softLight, s.lightSweep, s.sweepSpeed, 0.0f);
    f.toonF = glm::vec4(s.ditherFadeNear, s.ditherFadeFar, 0.0f, 0.0f);
    f.toonG = glm::vec4(s.pbrStyle, s.eyeFlatten, s.eyeParallax, s.eyeMatcap);
    f.toonH = glm::vec4(s.eyeHighlight, 0.0f, 0.0f, 0.0f);
    f.toonI = s.occlusionTarget;
    // Water (item 16): wave and colour settings for water.vert and water.frag. The underwater flag is set when the
    // camera is inside the volume and below its level.
    const bool hasWater = water_.has_value();
    bool underWater = false;
    if (hasWater) {
        const WaterVolume& wv = *water_;
        f.waterArea = glm::vec4(wv.centre.x, wv.level, wv.centre.y, 1.0f);
        f.waterHalf = glm::vec4(wv.half.x, wv.foamWidth, wv.half.y, wv.refraction);
        f.waterShallow = glm::vec4(wv.shallow, wv.absorption);
        for (int i = 0; i < 4; ++i) {
            const WaterWave& w = wv.waves[i];
            const float dl = glm::length(w.dir);
            const glm::vec2 d = dl > 1e-6f ? w.dir / dl : glm::vec2(1.0f, 0.0f);
            f.waterWaveD[i] = glm::vec4(d, w.wavelength, w.amplitude);
            f.waterWaveS[i] = glm::vec4(w.speed, w.steepness, 0.0f, 0.0f);
        }
        underWater = eye.y < wv.level && waterContainsXZ(wv, eye.x, eye.z);
        f.waterDeep = glm::vec4(wv.deep, underWater ? 1.0f : 0.0f);
    }
    f.pointColor[0] = glm::vec4(0.9f, 0.35f, 0.5f, 0.0f) * 0.4f;
    f.pointPos[1] = glm::vec4(-1.6f, 1.3f, 1.0f, 3.0f);
    f.pointColor[1] = glm::vec4(0.35f, 0.6f, 1.0f, 0.0f) * 0.35f;
    frameUbo_->upload(&f, sizeof(f));

    // Gather draws. A model past its cull distance is skipped. Each static opaque draw is then tested against the
    // camera frustum: `opaque` keeps every draw for the shadow pass, and `colorOpaque` keeps the ones in view for
    // the colour passes. Skinned draws are never frustum-culled, because the rest bounds do not follow the animation.
    constexpr float kCullMargin = 0.25f;   // metres, covers morph targets that move past the rest bounds
    std::vector<Rec> opaque, colorOpaque, blended, outlined, mmdEdges, graph, graphBlend, graphOutlined;
    std::deque<VGeomCut> vgeomCuts;   // stable addresses: Rec keeps a pointer into this
    uint32_t vgWorkU = 0;             // frame work-buffer cursor in uints (GPU cut only)
    for (Model* m : models) {
        if (!m) continue;
        const glm::vec3 centre = glm::vec3(m->root * glm::vec4(0.5f * (m->boundsMin + m->boundsMax), 1.0f));
        const DistanceVisibility dv = distanceVisibility(glm::length(cam.eye() - centre), m->fadeDistance, m->cullDistance);
        if (!dv.visible) continue;
        const bool boundsOk = glm::all(glm::greaterThan(m->boundsMax, m->boundsMin));
        for (const DrawItem& d : m->draws) {
            const Primitive* prim = &m->primitives[d.primitive];
            const Material* mat = prim->material;
            Rec r{m, d.node, d.skin, prim, dv.fade, true};
            // Opt-in LOD (item 15): static draws pick their level by distance from the eye to the model centre.
            if (d.skin < 0 && !m->lodSwitch.empty())
                r.lod = lodForDistance(glm::length(cam.eye() - centre), m->lodSwitch);
            // Virtual geometry: choose the cut once per draw, before the lists copy the record, so every pass this
            // frame draws the same clusters. Runs of adjacent clusters collapse into one indexed draw.
            if (s.vgeom && prim->hasVGeom()) {
                vgeomCuts.emplace_back();
                VGeomCut& cut = vgeomCuts.back();
                if (vgGpu_) {
                    // Compute cut: this draw becomes one dispatch. Its region of the frame work buffer, in
                    // uints: [idCount, cmdCount][ids pow2][commands cap*5] — headers and command tail are
                    // zeroed by the upload below; the kernel rewrites everything it uses.
                    const uint32_t count = uint32_t(prim->vgeom.clusters.size());
                    cut.gpu = true;
                    cut.prim = prim;
                    cut.model = m->nodes[d.node].world;
                    cut.pow2 = ceilPow2(count);
                    cut.idBase = vgWorkU + 2;
                    cut.cmdBase = cut.idBase + cut.pow2;
                    cut.cmdCap = count;
                    vgWorkU = cut.cmdBase + count * 5;
                } else {
                    VGeomStats stats;
                    selectVGeom(prim->vgeom, m->nodes[d.node].world, vgView, s.vgeomPixels, cut.clusters, &stats);
                    vgeomStats_.clusters += stats.clusters;
                    vgeomStats_.leaves += stats.leaves;
                    vgeomStats_.triangles += stats.triangles;
                    vgeomStats_.culled += stats.culled;
                    std::sort(cut.clusters.begin(), cut.clusters.end(), [&](uint32_t a, uint32_t b) {
                        return prim->vgeom.clusters[a].firstIndex < prim->vgeom.clusters[b].firstIndex;
                    });
                    for (uint32_t id : cut.clusters) {
                        const VGeomCluster& cl = prim->vgeom.clusters[id];
                        if (!cut.runs.empty() && cut.runs.back().first + cut.runs.back().second == cl.firstIndex)
                            cut.runs.back().second += cl.indexCount;
                        else
                            cut.runs.emplace_back(cl.firstIndex, cl.indexCount);
                    }
                }
                r.vgeomCut = &cut;
            }
            if (graphMaterials_.count(prim)) {
                GraphMaterial* gm = graphMaterials_.at(prim);
                (gm->transparent() ? graphBlend : graph).push_back(r);
                if (!gm->transparent() && s.outlines && gm->outlineOn()) graphOutlined.push_back(r);
                continue;
            }
            if (mat->blend) {
                blended.push_back(r);
                continue;
            }
            if (boundsOk && d.skin < 0 && s.frustumCull &&
                boxOutsideFrustum(viewProjUnjittered, m->nodes[d.node].world, m->boundsMin, m->boundsMax, kCullMargin))
                r.inView = false;
            opaque.push_back(r);
            if (!r.inView) continue;
            colorOpaque.push_back(r);
            if (mmdToon_.count(prim)) {
                if (s.outlines) mmdEdges.push_back(r);
            } else if (s.outlines && mat->outlined()) {
                outlined.push_back(r);
            }
        }
    }

    // The compute cut (docs/VGEOM.md §7): upload the frame's work buffer — zeroed headers and command tails, since
    // the kernel fills what it uses and the draws read every other slot as a zero command that draws nothing —
    // upload the selection view, then dispatch one thread per vgeom item. Device::dispatch brackets every call
    // with barriers, so the next item's atomics and the indirect draws below both see each region's writes. The
    // work buffer only ever grows, and a growth waits for idle first: the sets bind it by pointer.
    if (vgGpu_ && !vgeomCuts.empty()) {
        const size_t workBytes = size_t(vgWorkU) * 4;
        if (!vgWork_ || vgWork_->size() < workBytes) {
            dev_.waitIdle();
            vgWork_ = dev_.createBuffer(rhi::BufferUsage::Storage, workBytes);
            vgSets_.clear();
        }
        if (!vgView_) vgView_ = dev_.createBuffer(rhi::BufferUsage::Storage, sizeof(VgGpuView));
        std::vector<uint8_t> zeros(workBytes, 0);
        vgWork_->upload(zeros.data(), workBytes);
        VgGpuView view{};
        fillVgView(vgView, s.vgeomPixels, &view);
        vgView_->upload(&view, sizeof(view));
        dev_.bindPipeline(pVGeomCut_.get());
        for (VGeomCut& cut : vgeomCuts) {
            if (!cut.gpu) continue;
            auto it = vgSets_.find(cut.prim);
            if (it == vgSets_.end()) {
                auto set = dev_.createDescriptorSet(rhi::SetType::Compute);
                set->setBuffer(0, cut.prim->vgeomClusters.get());
                set->setBuffer(1, cut.prim->vgeomChildren.get());
                set->setBuffer(2, vgWork_.get());
                set->setBuffer(3, vgView_.get());
                it = vgSets_.emplace(cut.prim, std::move(set)).first;
            }
            dev_.bindDescriptorSet(0, it->second.get());
            VgPush push;
            push.model = cut.model;
            push.idBase = cut.idBase;
            push.cmdBase = cut.cmdBase;
            push.pow2 = cut.pow2;
            push.root = cut.prim->vgeom.root;
            dev_.pushConstants(&push, sizeof(push));
            dev_.dispatch(1, 1, 1);
        }
    }

    auto drawWith = [&](const Rec& r, rhi::Pipeline* pipe, rhi::DescriptorSet* frameSet,
                        rhi::DescriptorSet* materialSet, glm::vec4 p0) {
        const bool skinned = r.skin >= 0;
        const glm::mat4 model = skinned ? glm::mat4(1.0f) : r.model->nodes[r.node].world;
        rhi::DescriptorSet* skinSet = skinned ? r.model->skins[r.skin].set.get() : r.model->staticSkinSet.get();
        p0.x = skinned ? 1.0f : 0.0f;
        const auto dit = motionDisp_.find(r.model);
        const glm::vec4 disp = dit == motionDisp_.end() ? glm::vec4(0.0f) : glm::vec4(dit->second, 0.0f);
        PushConstants pc = pcWith(model, p0, disp);
        pc.p2 = glm::vec4(r.fade, 0.0f, 0.0f, 0.0f);
        dev_.bindPipeline(pipe);
        dev_.bindDescriptorSet(0, frameSet);
        dev_.bindDescriptorSet(1, materialSet);
        dev_.bindDescriptorSet(2, skinSet);
        rhi::Buffer* ib = nullptr;
        uint32_t ic = 0;
        lodIndices(*r.prim, r.lod, &ib, &ic);
        dev_.bindVertexBuffer(r.prim->vertices.get());
        // A cut replaces the distance LOD in every pass except the shadow one: a shadow keeps the classic index
        // buffer, so a coarse parent can never open a gap in a shadow map.
        const bool vgeom = r.vgeomCut != nullptr && pipe != pShadow_.get();
        dev_.bindIndexBuffer(vgeom ? r.prim->vgeomIndices.get() : ib);
        dev_.pushConstants(&pc, sizeof(pc));
        if (vgeom) {
            if (r.vgeomCut->gpu) {
                // One indirect draw over the region the kernel wrote: each entry is a merged run of clusters,
                // every unwritten slot a zero command. Triangle totals would need a readback we will not pay
                // for mid-frame, so this path counts the call and leaves triangles_ to the CPU path's numbers.
                dev_.drawIndexedIndirect(vgWork_.get(), size_t(r.vgeomCut->cmdBase) * 4, r.vgeomCut->cmdCap);
                ++drawCalls_;
            } else {
                for (const auto& run : r.vgeomCut->runs) {
                    dev_.drawIndexed(run.second, run.first);
                    ++drawCalls_;
                    triangles_ += run.second / 3;
                }
            }
        } else {
            dev_.drawIndexed(ic);
            ++drawCalls_;
            triangles_ += ic / 3;
        }
    };
    auto bindAndDraw = [&](const Rec& r, rhi::Pipeline* pipe, rhi::DescriptorSet* frameSet, glm::vec4 p0) {
        drawWith(r, pipe, frameSet, r.prim->material->set.get(), p0);
    };

    // The GPU brackets the device resolved when this frame began: one frame behind, never a wait (docs/PROFILER.md).
    gpuStats_.clear();
    if (const std::vector<float> gpu = dev_.gpuTimesMs(); !gpu.empty()) {
        static const char* kGpuNames[] = {"shadows", "depth", "scene", "post", "hud"};
        const size_t n = std::min(gpu.size(), sizeof(kGpuNames) / sizeof(kGpuNames[0]));
        for (size_t i = 0; i < n; ++i) gpuStats_.push_back({kGpuNames[i], gpu[i]});
    }

    // 1. Cascaded shadow atlas. The pass always runs so the atlas is cleared.
    {
        dev_.beginGpuTime(0);
        rhi::PassDesc sp;
        sp.colors.clear();
        sp.depth = shadowAtlas_.get();
        sp.clearColor = false;
        sp.clearDepth = true;
        dev_.beginPass(sp);
        if (s.shadows) {
            for (int c = 0; c < 3; ++c) {
                dev_.setViewport((c % 2) * kCascadeSize, (c / 2) * kCascadeSize, kCascadeSize, kCascadeSize);
                for (const Rec& r : opaque)   // blended materials do not cast shadows
                    bindAndDraw(r, pShadow_.get(), shadowFrameSet_.get(), glm::vec4(0.0f, float(c), 0.0f, 0.0f));
                // Terrain casts too (item 19): without it a hillside has no shadow of its own, and grass on the dark
                // side of a ridge stays lit. Chunks outside this cascade's light frustum are skipped.
                if (!terrain_.empty()) {
                    dev_.bindPipeline(pTerrainShadow_.get());
                    dev_.bindDescriptorSet(0, shadowFrameSet_.get());
                    for (const TerrainDraw& t : terrain_) {
                        if (boxOutsideFrustum(f.shadowMatrix[c], t.model, t.boundsMin, t.boundsMax, kCullMargin)) continue;
                        PushConstants pc = pcWith(t.model, glm::vec4(float(c), 0.0f, 0.0f, 0.0f));
                        dev_.bindVertexBuffer(t.vertices);
                        dev_.bindIndexBuffer(t.indices);
                        dev_.pushConstants(&pc, sizeof(pc));
                        dev_.drawIndexed(t.indexCount);
                    }
                }
            }
        }
        dev_.endPass();
        dev_.endGpuTime(0);
    }

    dev_.beginGpuTime(1);
    // 2a. Depth prepass: the in-view opaque toon draws write depth first, so the colour pass shades each pixel once
    // (its depth test is LESS_OR_EQUAL, so equal depths pass). The depth shader runs the colour shaders' discards.
    if (s.depthPrepass) {
        rhi::PassDesc dp;
        dp.depth = depth_.get();
        dp.clearDepth = true;
        dev_.beginPass(dp);
        dev_.setViewport(0, 0, intW_, intH_);
        for (const Rec& r : colorOpaque)
            bindAndDraw(r, pDepth_[r.prim->material->doubleSided ? 1 : 0].get(), frameSet_.get(), glm::vec4(0.0f));
        dev_.endPass();
    }

    dev_.endGpuTime(1);

    // Blended draws. Without water they follow the opaque draws in the scene pass. With water they run after the
    // water surface, in a pass of their own.
    auto drawBlended = [&]() {
        for (const Rec& r : blended) {
            const int dbl = r.prim->material->doubleSided ? 1 : 0;
            rhi::Pipeline* pipe = mmdToon_.count(r.prim) ? pMmdToon_[1][dbl].get() : pMToon_[1][dbl].get();
            bindAndDraw(r, pipe, frameSet_.get(), glm::vec4(0.0f));
        }
        for (const Rec& r : graphBlend)
            drawWith(r, graphMaterials_.at(r.prim)->pipeline(), frameSet_.get(),
                     graphMaterials_.at(r.prim)->set(), glm::vec4(0.0f));
    };

    // Trails (item 27): one indexed draw for the alpha list and one for the additive list, over the geometry the CPU
    // already built. Created on first use so a scene with no trails pays nothing; both target the HDR colourbuffer
    // with the scene depth attached, which is the pass this is called from.
    auto drawTrails = [&]() {
        if (trailAlpha_.empty() && trailAdditive_.empty()) return;
        if (!trailAlphaRenderer_)
            trailAlphaRenderer_ = std::make_unique<TrailRenderer>(dev_, rhi::Format::RGBA16F, true, TrailBlend::Alpha);
        if (!trailAdditiveRenderer_)
            trailAdditiveRenderer_ = std::make_unique<TrailRenderer>(dev_, rhi::Format::RGBA16F, true, TrailBlend::Additive);
        const uint32_t alphaTris = trailAlphaRenderer_->draw(dev_, *frameSet_, eye, trailAlpha_);
        if (alphaTris > 0) { ++drawCalls_; triangles_ += alphaTris; }
        const uint32_t addTris = trailAdditiveRenderer_->draw(dev_, *frameSet_, eye, trailAdditive_);
        if (addTris > 0) { ++drawCalls_; triangles_ += addTris; }
        trailTriangles_ = alphaTris + addTris;
    };

    dev_.beginGpuTime(2);
    // 2. Scene: sky, infinite grid, toon materials, outlines, then blended materials.
    {
        rhi::PassDesc hp;
        hp.colors = {hdr_.get()};
        hp.depth = depth_.get();
        hp.clearColor = true;
        hp.clearValue = glm::vec4(0.8f, 0.9f, 1.0f, 1.0f);
        hp.clearDepth = !s.depthPrepass;   // the prepass already cleared it
        dev_.beginPass(hp);
        dev_.setViewport(0, 0, intW_, intH_);
        if (s.sky) {
            dev_.bindPipeline(pSky_.get());
            dev_.bindDescriptorSet(0, frameSet_.get());
            dev_.drawFullscreen();
        }
        // Terrain (item 18). Drawn before the grid, so the grid's depth test sees the hills.
        if (!terrain_.empty()) {
            dev_.bindPipeline(pTerrain_.get());
            dev_.bindDescriptorSet(0, frameSet_.get());
            for (const TerrainDraw& t : terrain_) {
                if (boxOutsideFrustum(viewProjUnjittered, t.model, t.boundsMin, t.boundsMax, kCullMargin)) continue;
                PushConstants pc = pcWith(t.model, glm::vec4(0.0f));
                if (t.layers) dev_.bindDescriptorSet(1, t.layers);
                dev_.bindVertexBuffer(t.vertices);
                dev_.bindIndexBuffer(t.indices);
                dev_.pushConstants(&pc, sizeof(pc));
                dev_.drawIndexed(t.indexCount);
                ++drawCalls_;
                triangles_ += t.indexCount / 3;
            }
        }
        // Foliage (item 18, step 3). Each batch is one instanced draw. A batch is skipped when its chunk is outside the
        // view, or when the whole chunk is past the distance where its foliage has faded out.
        if (!foliage_.empty()) {
            dev_.bindPipeline(pFoliage_.get());
            dev_.bindDescriptorSet(0, frameSet_.get());
            for (const FoliageDraw& f : foliage_) {
                const glm::vec3 centre = glm::vec3(f.model * glm::vec4((f.boundsMin + f.boundsMax) * 0.5f, 1.0f));
                const float radius = glm::length(f.boundsMax - f.boundsMin) * 0.5f;
                if (glm::distance(eye, centre) - radius > f.fade.y) continue;
                if (boxOutsideFrustum(viewProjUnjittered, f.model, f.boundsMin, f.boundsMax, kCullMargin)) continue;
                PushConstants pc = pcWith(f.model, glm::vec4(f.fade, 0.0f, 0.0f), glm::vec4(f.tint, 0.0f));
                dev_.bindDescriptorSet(1, f.instances);
                dev_.bindVertexBuffer(f.vertices);
                dev_.bindIndexBuffer(f.indices);
                dev_.pushConstants(&pc, sizeof(pc));
                dev_.drawIndexedInstanced(f.indexCount, f.instanceCount);
                ++drawCalls_;
                triangles_ += (f.indexCount / 3) * f.instanceCount;
            }
        }
        // Grass (item 19). The same culls as foliage: the batch's chunk against the frustum, and the whole chunk
        // against the distance where the last blade has faded. Inside a batch the vertex shader thins blades by
        // distance and bends them in the wind.
        if (!grass_.empty()) {
            dev_.bindPipeline(pGrass_.get());
            dev_.bindDescriptorSet(0, frameSet_.get());
            for (const GrassDraw& g : grass_) {
                const glm::vec3 centre = glm::vec3(g.model * glm::vec4((g.boundsMin + g.boundsMax) * 0.5f, 1.0f));
                const float radius = glm::length(g.boundsMax - g.boundsMin) * 0.5f;
                if (glm::distance(eye, centre) - radius > g.bands.z) continue;
                if (boxOutsideFrustum(viewProjUnjittered, g.model, g.boundsMin, g.boundsMax, kCullMargin)) continue;
                PushConstants pc = pcWith(g.model, glm::vec4(g.bands, 0.0f), g.wind);
                pc.p2 = g.pusher;
                pc.p3 = glm::vec4(g.windDir, 0.0f, 0.0f);
                dev_.bindDescriptorSet(1, g.instances);
                dev_.bindVertexBuffer(g.vertices);
                dev_.bindIndexBuffer(g.indices);
                dev_.pushConstants(&pc, sizeof(pc));
                dev_.drawIndexedInstanced(g.indexCount, g.instanceCount);
                ++drawCalls_;
                triangles_ += (g.indexCount / 3) * g.instanceCount;
            }
        }
        if (s.grid) {
            dev_.bindPipeline(hasWater ? pGridWater_.get() : pGrid_.get());
            dev_.bindDescriptorSet(0, frameSet_.get());
            dev_.drawFullscreen();
        }
        for (const Rec& r : colorOpaque) {
            const int dbl = r.prim->material->doubleSided ? 1 : 0;
            rhi::Pipeline* pipe = mmdToon_.count(r.prim) ? pMmdToon_[0][dbl].get() : pMToon_[0][dbl].get();
            bindAndDraw(r, pipe, frameSet_.get(), glm::vec4(0.0f));
        }
        for (const Rec& r : graph)
            drawWith(r, graphMaterials_.at(r.prim)->pipeline(), frameSet_.get(),
                     graphMaterials_.at(r.prim)->set(), glm::vec4(0.0f));
        // Graph materials with an outline: the inverted hull reads the material's own outline slots.
        for (const Rec& r : graphOutlined)
            drawWith(r, graphMaterials_.at(r.prim)->outlinePipeline(), frameSet_.get(),
                     graphMaterials_.at(r.prim)->set(), glm::vec4(0.0f));
        for (const Rec& r : outlined)
            bindAndDraw(r, pOutline_.get(), frameSet_.get(), glm::vec4(0.0f, s.outlineScale, 0.0f, 0.0f));
        for (const Rec& r : mmdEdges)
            bindAndDraw(r, pMmdEdge_.get(), frameSet_.get(), glm::vec4(0.0f, s.outlineScale, 0.0f, 0.0f));
        if (!hasWater) drawBlended();
        drawTrails();
        dev_.endPass();
    }

    // 2w. Water (item 16). The scene so far (sky and opaque) is copied to refr_ with its view depth. The surface is
    // then drawn depth-tested and without depth writes. Blended draws follow, then the underwater fog if the camera
    // is under the surface.
    if (hasWater) {
        const uint32_t rw = std::max(1u, intW_ / 2), rh = std::max(1u, intH_ / 2);
        {
            rhi::PassDesc cp;
            cp.colors = {refr_.get()};
            cp.clearColor = false;
            cp.depth = nullptr;
            cp.clearDepth = false;
            dev_.beginPass(cp);
            dev_.setViewport(0, 0, rw, rh);
            dev_.bindPipeline(pWaterCopy_.get());
            dev_.bindDescriptorSet(0, waterCopySet_.get());
            dev_.drawFullscreen();
            dev_.endPass();
        }
        {
            rhi::PassDesc wp;
            wp.colors = {hdr_.get()};
            wp.depth = depth_.get();
            wp.clearColor = false;
            wp.clearDepth = false;
            dev_.beginPass(wp);
            dev_.setViewport(0, 0, intW_, intH_);
            dev_.bindPipeline(pWater_.get());
            dev_.bindDescriptorSet(0, frameSet_.get());
            dev_.bindDescriptorSet(1, waterSet_.get());
            dev_.bindVertexBuffer(waterVb_.get());
            dev_.bindIndexBuffer(waterIb_.get());
            dev_.drawIndexed(waterIndexCount_);
            dev_.endPass();
            ++drawCalls_;
            triangles_ += waterIndexCount_ / 3;
        }
        {
            rhi::PassDesc bp;
            bp.colors = {hdr_.get()};
            bp.depth = depth_.get();
            bp.clearColor = false;
            bp.clearDepth = false;
            dev_.beginPass(bp);
            dev_.setViewport(0, 0, intW_, intH_);
            drawBlended();
            drawTrails();
            dev_.endPass();
        }
        if (underWater) {
            rhi::PassDesc up;
            up.colors = {hdr_.get()};
            up.clearColor = false;
            up.depth = nullptr;
            up.clearDepth = false;
            dev_.beginPass(up);
            dev_.setViewport(0, 0, intW_, intH_);
            dev_.bindPipeline(pUnderwater_.get());
            dev_.bindDescriptorSet(0, waterSet_.get());
            dev_.drawFullscreen();
            dev_.endPass();
        }
    }

    // Helper for full-target post passes.
    auto post = [&](rhi::Texture* target, bool clear, rhi::Pipeline* pipe, rhi::DescriptorSet* set,
                    const PushConstants& pc, uint32_t vw, uint32_t vh) {
        rhi::PassDesc pd;
        pd.colors = {target};
        pd.clearColor = clear;
        pd.clearValue = glm::vec4(0.0f);
        pd.depth = nullptr;
        pd.clearDepth = false;
        dev_.beginPass(pd);
        dev_.setViewport(0, 0, vw, vh);
        dev_.bindPipeline(pipe);
        dev_.bindDescriptorSet(0, set);
        dev_.pushConstants(&pc, sizeof(pc));
        dev_.drawFullscreen();
        dev_.endPass();
    };

    // 2a. Per-object motion vectors (TAA, motion blur, or the GI history). Opaque geometry is drawn again against the
    // scene depth
    // with this frame's transforms and last frame's (joint palette, or model matrix, plus the previous
    // unjittered viewProj). Camera motion is included, so a still object reprojects as the camera does.
    // Pixels with no opaque geometry keep a zero vector and fall back to camera reprojection in TAA.
    // The first frame has no previous view: start from this one so the velocity is zero, not garbage.
    if (!prevViewReady_) {
        prevViewProj_ = viewProjUnjittered;
        prevViewReady_ = true;
    }
    // The GI accumulator reprojects its history with this buffer, so GI needs it even when TAA and motion blur are
    // both off — otherwise it would read whatever the last frame that had TAA left behind.
    const bool needVelocity = taaOn || s.motionBlur > 0.0f || s.gi > 0.0f;
    if (needVelocity) {
        if (taaReset)   // no valid history (first frame, or TAA just switched on): no previous transforms
            for (auto& entry : motion_) entry.second.last.clear();

        const glm::mat4 identity(1.0f);
        // Prepare every draw's data before recording, so each slot holds exactly this frame's values.
        for (const Rec& r : colorOpaque) {
            const bool skinned = r.skin >= 0;
            const std::pair<const void*, int> key{r.model, skinned ? -(r.skin + 1) : r.node};
            auto it = motion_.find(key);
            if (it == motion_.end()) {
                MotionSlot slot;
                slot.ubo = dev_.createBuffer(rhi::BufferUsage::Uniform, sizeof(MotionUBO));
                slot.set = dev_.createDescriptorSet(rhi::SetType::Motion);
                slot.set->setBuffer(10, slot.ubo.get());
                it = motion_.emplace(key, std::move(slot)).first;
            }
            MotionSlot& slot = it->second;
            slot.current.clear();
            if (skinned) {
                const Skin& sk = r.model->skins[r.skin];
                for (size_t j = 0; j < sk.joints.size() && j < 128; ++j)
                    slot.current.push_back(r.model->nodes[sk.joints[j]].world * sk.inverseBind[j]);
            } else {
                slot.current.push_back(r.model->nodes[r.node].world);
            }
            const std::vector<glm::mat4>& prev = slot.last.empty() ? slot.current : slot.last;
            MotionUBO mb{};
            for (auto& jm : mb.joints) jm = identity;
            for (size_t j = 0; j < prev.size() && j < 128; ++j) mb.joints[j] = prev[j];
            mb.prevViewProj = prevViewProj_;
            slot.ubo->upload(&mb, sizeof(mb));
        }

        rhi::PassDesc vp;
        vp.colors = {velocity_.get()};
        vp.depth = depth_.get();
        vp.clearColor = true;
        vp.clearValue = glm::vec4(0.0f);
        vp.clearDepth = false;     // keep the scene depth so only visible surfaces are written
        dev_.beginPass(vp);
        dev_.setViewport(0, 0, intW_, intH_);
        dev_.bindPipeline(pVelocity_.get());
        for (const Rec& r : colorOpaque) {
            const bool skinned = r.skin >= 0;
            const std::pair<const void*, int> key{r.model, skinned ? -(r.skin + 1) : r.node};
            MotionSlot& slot = motion_.at(key);
            const glm::mat4 model = skinned ? identity : r.model->nodes[r.node].world;
            rhi::DescriptorSet* skinSet = skinned ? r.model->skins[r.skin].set.get() : r.model->staticSkinSet.get();
            PushConstants pc = pcWith(model, glm::vec4(skinned ? 1.0f : 0.0f, 0, 0, 0));
            dev_.bindDescriptorSet(0, frameSet_.get());
            dev_.bindDescriptorSet(1, r.prim->material->set.get());
            dev_.bindDescriptorSet(2, skinSet);
            dev_.bindDescriptorSet(3, slot.set.get());
            dev_.bindVertexBuffer(r.prim->vertices.get());
            rhi::Buffer* ib = nullptr;
            uint32_t ic = 0;
            lodIndices(*r.prim, r.lod, &ib, &ic);
            dev_.bindIndexBuffer(ib);
            dev_.pushConstants(&pc, sizeof(pc));
            dev_.drawIndexed(ic);
            ++drawCalls_;
            triangles_ += ic / 3;
        }
        dev_.endPass();
        for (const Rec& r : colorOpaque) {
            const std::pair<const void*, int> key{r.model, r.skin >= 0 ? -(r.skin + 1) : r.node};
            MotionSlot& slot = motion_.at(key);
            slot.last = slot.current;   // becomes the previous frame for the next frame
        }
    }

    // 2a'. View-space normals for SSAO, SSR, contact shadows and the depth-normal outline. The same draws as the velocity
    // pass, against the scene depth. Pixels with no opaque surface keep alpha 0.
    const bool needNormals = s.ssao > 0.0f || s.ssr > 0.0f || s.depthNormalOutline > 0.0f || s.screenRim > 0.0f
                              || s.depthEdgeRim > 0.0f || s.gi > 0.0f || s.contactShadows > 0.0f;
    if (needNormals) {
        rhi::PassDesc np;
        np.colors = {normal_.get(), albedo_.get()};
        np.depth = depth_.get();
        np.clearColor = true;
        np.clearValue = glm::vec4(0.5f, 0.5f, 0.5f, 0.0f);
        np.clearDepth = false;
        dev_.beginPass(np);
        dev_.setViewport(0, 0, intW_, intH_);
        dev_.bindPipeline(pNormal_.get());
        for (const Rec& r : colorOpaque) {
            const bool skinned = r.skin >= 0;
            const glm::mat4 model = skinned ? glm::mat4(1.0f) : r.model->nodes[r.node].world;
            rhi::DescriptorSet* skinSet = skinned ? r.model->skins[r.skin].set.get() : r.model->staticSkinSet.get();
            PushConstants pc = pcWith(model, glm::vec4(skinned ? 1.0f : 0.0f, 0, 0, 0));
            dev_.bindDescriptorSet(0, frameSet_.get());
            dev_.bindDescriptorSet(1, r.prim->material->set.get());
            dev_.bindDescriptorSet(2, skinSet);
            dev_.bindVertexBuffer(r.prim->vertices.get());
            rhi::Buffer* ib = nullptr;
            uint32_t ic = 0;
            lodIndices(*r.prim, r.lod, &ib, &ic);
            dev_.bindIndexBuffer(ib);
            dev_.pushConstants(&pc, sizeof(pc));
            dev_.drawIndexed(ic);
            ++drawCalls_;
            triangles_ += ic / 3;
        }
        dev_.endPass();
    }

    dev_.endGpuTime(2);
    dev_.beginGpuTime(3);

    // 2b2. Real-time GI (the iGPU path tracer, docs/PATHTRACE.md §4). Two passes at half resolution: trace one bounce
    // per half-res pixel against the heightfield, accumulate it over frames, and leave the result in a texture the
    // effects pass composites at full resolution with a depth-guided bilateral filter. The rays read this frame's
    // raster (hdr_, before the effects pass runs), so the estimator never feeds its own output back into itself.
    const bool giOn = s.gi > 0.0f && heightfield_.height != nullptr && hdr_ && depth_ && normal_;
    bool giComposite = false;
    if (giOn) {
        const uint32_t gw = std::max(1u, intW_ / 2), gh = std::max(1u, intH_ / 2);
        // The trace's push constants: inverse viewProj in `model`, the heightfield and ray parameters, and the tile
        // origin (the shader works in metres from the tile's first sample).
        glm::mat4 invVP = glm::inverse(f.viewProj);
        PushConstants tp = pcWith(invVP, glm::vec4(heightfield_.cellSize, heightfield_.heightScale, s.giRadius, 0.05f),
                                  glm::vec4(std::clamp(s.giRays, 1.0f, 8.0f), 0.0f, 0.0f, 0.0f),
                                  glm::vec4(heightfield_.origin.x, heightfield_.origin.z, heightfield_.origin.y,
                                            float(giFrame_)));
        // The trace needs the Frame set too, so it is bound by hand rather than through post(), which binds only set 0.
        // The heightmap is pointer-stable for a tile but the camera can move to another tile, so it is (re)bound here.
        setGiTrace_->setTexture(2, heightfield_.height);
        {
            rhi::PassDesc pd;
            pd.colors = {gi_.get()};
            pd.clearColor = true;
            pd.clearValue = glm::vec4(0.0f);
            dev_.beginPass(pd);
            dev_.setViewport(0, 0, gw, gh);
            dev_.bindPipeline(pGiTrace_.get());
            dev_.bindDescriptorSet(0, frameSet_.get());
            dev_.bindDescriptorSet(1, setGiTrace_.get());
            dev_.pushConstants(&tp, sizeof(tp));
            dev_.drawFullscreen();
            dev_.endPass();
        }
        // Accumulate into the write history, reading the other one. The first frame and every tenth after it take the
        // trace as is, so a moving camera never lags more than a few frames of light.
        const float weight = (giFrame_ % 10 == 0) ? 1.0f : std::max(0.03125f, s.giHistory);
        // prevViewProj_ is still last frame's here: section 2b advances it after this pass. The gatherer needs it to
        // reproject the history of surfaces the velocity pass does not draw — the terrain, which is the scene.
        const glm::mat4 giReproj = prevViewProj_ * glm::inverse(f.viewProj);
        post(giHist_[giWrite_].get(), true, pGiAccum_.get(), setGiAccum_[giWrite_].get(),
             pcWith(giReproj, glm::vec4(weight, 0, 0, 0),
                    glm::vec4(1.0f / float(intW_), 1.0f / float(intH_), 0, 0)),
             int(intW_) / 2, int(intH_) / 2);
        // Denoise what the accumulator produced, into gi_ (the trace's own target, free until next frame). The
        // accumulation history itself stays unfiltered, so the filter can never feed its own output back into the
        // samples it is filtering; only the image the frame displays is denoised.
        rhi::Texture* giFinal = giHist_[giWrite_].get();
        if (s.giDenoise) {
            post(giDenoise_.get(), false, pGiDenoise_.get(), setGiDenoise_[giWrite_].get(),
                 pcWith(glm::mat4(1.0f), glm::vec4(1.0f, 0.02f, 16.0f, 0.0f)), gw, gh);
            post(gi_.get(), false, pGiDenoise_.get(), setGiDenoiseB_.get(),
                 pcWith(glm::mat4(1.0f), glm::vec4(2.0f, 0.02f, 16.0f, 0.0f)), gw, gh);
            giFinal = gi_.get();
        }
        // The effects pass composites it: binding 5 carries the accumulated GI, and p3.w its strength.
        setEffects_->setTexture(5, giFinal);
        giComposite = true;
        giWrite_ ^= 1;
        ++giFrame_;
    } else {
        giFrame_ = 0;
    }
    // Binding 6: the albedo the multi-bounce AO fit reads. Off, or unavailable this frame, the slot holds a 1x1
    // transparent texture and effects.frag falls back to single-bounce AO.
    setEffects_->setTexture(6, s.multiBounceAO && s.ssao > 0.0f ? albedo_.get() : noAlbedo_.get());

    // 2c. Screen-space effects on the HDR frame (shaders/effects.frag): motion blur, SSAO, depth-normal
    // outline, SSR, fog with sun in-scatter, lens flare. Runs before TAA, bloom and tonemap, which read
    // fxHdr_. With everything at its default the pass copies the frame.
    {
        glm::mat4 e(0.0f);
        e[0] = glm::vec4(projUnjittered[0][0], projUnjittered[1][1], projUnjittered[2][2], projUnjittered[3][2]);
        e[1] = glm::vec4(s.fogColor, s.fogDensity);
        const glm::vec3 sunView = glm::mat3(f.view) * L;
        e[2] = glm::vec4(sunView, s.fogSun);
        glm::vec2 sunUV(0.5f);
        float sunVisible = 0.0f;
        if (sunView.z < 0.0f) {   // the sun is in front of the camera: project a point far along it
            const glm::vec4 clip = projUnjittered * glm::vec4(sunView * 500.0f, 1.0f);
            sunUV = glm::vec2(clip.x / clip.w, clip.y / clip.w) * 0.5f + 0.5f;
            sunVisible = 1.0f;
        }
        e[3] = glm::vec4(sunUV, s.flare * sunVisible, s.ssao);
        // p1: SSAO radius (m), rim, outline noise, depth-edge rim. The traced GI strength rides in p3.w, the one slot
        // the split line leaves free: it is written before the split block below and that block preserves it.
        PushConstants ep = pcWith(e, glm::vec4(s.ssr, s.depthNormalOutline, s.motionBlur, aspect),
                                  glm::vec4(0.5f, s.screenRim, s.outlineNoise, s.depthEdgeRim));
        // SplitScreen line: p2 = (dir, offset px, width px); p3 = tints and line colour as packed RGBA8 bits.
        ep.p3.w = giComposite ? s.gi : 0.0f;
        // Contact-shadow parameters: Post binding 7, uploaded here because the push block has no free slot.
        const glm::vec4 contactParams(s.contactShadowLength, s.contactShadowThickness, s.contactShadows,
                                      float(s.contactShadowSteps));
        contactUbo_->upload(&contactParams, sizeof(contactParams));
        if (glm::dot(s.splitDir, s.splitDir) > 1e-8f) {
            const glm::vec2 dir = glm::normalize(s.splitDir);
            ep.p2 = glm::vec4(dir, s.splitOffsetPx, s.splitLineWidthPx);
            const float keepGi = ep.p3.w;
            ep.p3 = glm::vec4(packBits(s.splitNprTint), packBits(s.splitPbrTint), packBits(s.splitLineColor), keepGi);
        }
        post(fxHdr_.get(), true, pEffects_.get(), setEffects_.get(), ep, intW_, intH_);
    }

    // 2d. Auto exposure (shaders/expo_reduce.frag). The reduce moves expo_[0] towards the target
    // multiplier; the copy keeps expo_[1] as the previous state for the next frame. Tonemap reads expo_[0].
    {
        post(expo_[0].get(), true, pExpo_.get(), setExpoReduce_.get(),
             pcWith(glm::mat4(1.0f), glm::vec4(s.autoKey, s.autoExposure ? 1.0f : 0.0f, 0.2f, 0.0f)), 1, 1);
        post(expo_[1].get(), true, pExpo_.get(), setExpoCopy_.get(),
             pcWith(glm::mat4(1.0f), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)), 1, 1);
    }

    // 2b. TAA resolve (AA mode 4). Reprojects the previous resolved frame with the depth buffer and
    // the previous unjittered viewProj, clamps it to this frame's neighbourhood and blends in this
    // frame's jittered sample. Bloom and tonemap then read the resolved target.
    const glm::mat4 reprojPrev = prevViewProj_;
    prevViewProj_ = viewProjUnjittered;
    uint32_t resolvedIdx = taaWrite_;
    if (taaOn) {
        const uint32_t w = taaWrite_;
        const glm::mat4 reproj = reprojPrev * glm::inverse(f.viewProj);
        PushConstants pc = pcWith(reproj, glm::vec4(0.1f, 0, 0, 0),
                                  glm::vec4(1.0f / float(intW_), 1.0f / float(intH_), 0, 0));
        if (taaReset) pc.p0.x = 1.0f;   // no usable history: take the current frame
        post(taaHist_[w].get(), true, pTaa_.get(), setTaa_[w].get(), pc, intW_, intH_);
        resolvedIdx = w;
        taaWrite_ = 1 - w;
    }
    taaHistoryValid_ = taaOn;

    // 3. Bloom, as in Kaze's UnityGenshinPostProcessing (research/kaze_post_port.md): a prefilter (13-tap
    //    downsample, then the threshold), a 9-tap separable Gaussian per level, then a weighted sum of the four
    //    levels with the glow tint. The intensity (bloomStrength) is applied in tonemap.
    if (s.bloom) {
        const float srcW = float(intW_), srcH = float(intH_);
        post(bloom_[0].get(), true, pBloomDown_.get(), taaOn ? setDownTaa_[resolvedIdx].get() : setDown_[0].get(),
             pcWith(glm::mat4(1.0f), glm::vec4(s.bloomThreshold, float(glm::clamp(s.bloomThresholdMode, 0, 2)), 0, 0),
                    glm::vec4(1.0f / srcW, 1.0f / srcH, 0, 0)),
             bloom_[0]->width(), bloom_[0]->height());

        // Kaze's blur radius: blurRadius * downSampleScale * (frame height / 1080).
        const float radius = glm::max(s.bloomBlurRadius, 0.0f) * bloomScale_ * (srcH / 1080.0f);
        for (int i = 0; i < 4; ++i) {
            // Level 0 blurs the prefilter output; level i reads level i-1 at twice the spacing (Kaze's H2x).
            rhi::Texture* src = bloom_[i == 0 ? 0 : i - 1].get();
            const float spacing = (i == 0) ? 1.0f : 2.0f;
            post(bloomTmp_[i].get(), true, pBloomBlur_.get(), setBlurH_[i].get(),
                 pcWith(glm::mat4(1.0f), glm::vec4(radius * spacing, 0, 0, 0),
                        glm::vec4(1.0f / src->width(), 1.0f / src->height(), 0, 0)),
                 bloomTmp_[i]->width(), bloomTmp_[i]->height());
            rhi::Texture* tmp = bloomTmp_[i].get();
            post(bloom_[i].get(), true, pBloomBlur_.get(), setBlurV_[i].get(),
                 pcWith(glm::mat4(1.0f), glm::vec4(0.0f, radius, 0, 0),
                        glm::vec4(1.0f / tmp->width(), 1.0f / tmp->height(), 0, 0)),
                 bloom_[i]->width(), bloom_[i]->height());
        }

        // Weighted sum of the four levels, then the glow tint in model[0] (w = 1 flags it).
        glm::mat4 tintModel(1.0f);
        tintModel[0] = glm::vec4(glm::max(s.bloomTint, glm::vec3(0.0f)), 1.0f);
        post(bloomOut_.get(), true, pBloomCombine_.get(), setCombine_.get(),
             pcWith(tintModel, glm::max(s.bloomWeights, glm::vec4(0.0f))),
             bloomOut_->width(), bloomOut_->height());
    } else {
        // Clear bloomOut_ to black so the tonemap pass never reads undefined data.
        rhi::PassDesc cp;
        cp.colors = {bloomOut_.get()};
        cp.clearColor = true;
        cp.clearValue = glm::vec4(0.0f);
        cp.clearDepth = false;
        dev_.beginPass(cp);
        dev_.endPass();
    }

    // 4. Tonemap (ACES-like curve, saturation/contrast, bloom add, vignette, grain, sRGB encode).
    // The colour grade travels in pc.model, which this pass does not otherwise read:
    // model[0] shadow tint, model[1] highlight tint, model[2].x white balance, model[3].x LUT amount.
    glm::mat4 grade(0.0f);
    grade[0] = glm::vec4(s.shadowTint, 0.0f);
    grade[1] = glm::vec4(s.highlightTint, 0.0f);
    // model[2]: x white balance, y chromatic aberration, z sharpen, w auto exposure on.
    grade[2] = glm::vec4(glm::clamp(s.whiteBalance, -1.0f, 1.0f), glm::clamp(s.chromatic, 0.0f, 1.0f),
                         glm::clamp(s.sharpen, 0.0f, 1.0f), s.autoExposure ? 1.0f : 0.0f);
    grade[3] = glm::vec4(hasLut_ ? glm::clamp(s.lutAmount, 0.0f, 1.0f) : 0.0f,
                         glm::clamp(s.animeKnee, 0.3f, 0.95f), glm::clamp(s.animeToe, 0.0f, 0.05f),
                         s.dither ? 1.0f : 0.0f);
    post(ldr0_.get(), true, pTonemap_.get(), taaOn ? setTonemapTaa_[resolvedIdx].get() : setTonemap_.get(),
         pcWith(grade, glm::vec4(s.exposure, s.bloomStrength, s.saturation, s.contrast),
                glm::vec4(s.vignette, s.grain, time,
                          float(s.tonemap < 0 ? 0 : (s.tonemap > 10 ? 10 : s.tonemap))
                          + 16.0f * float(glm::clamp(s.look, 0, 3))
                          + 64.0f * (s.contrastLogC ? 1.0f : 0.0f)
                          + 128.0f * (s.saturationLinear ? 1.0f : 0.0f))),
         intW_, intH_);

    // 5. Stylize (optional): reads ldr0 and writes ldr1. The outlines need eye depth, which is
    // P32 / (d + P22) for this projection, so the two terms travel in model[0].x and .y.
    const bool doStylize = s.stylize > 0;
    if (doStylize) {
        glm::mat4 projTerms(0.0f);
        projTerms[0] = glm::vec4(f.proj[2][2], f.proj[3][2], 0.0f, 0.0f);
        post(ldr1_.get(), true, pStylize_.get(), setStylize_.get(),
             pcWith(projTerms, glm::vec4(float(s.stylize), s.stylizeStrength, s.stylizeOutline, 0.0f),
                    glm::vec4(float(intW_), float(intH_), time, 0.0f)),
             intW_, intH_);
    }

    // 6. FXAA (AA modes 1 and 3). It reads whichever LDR target holds the frame.
    const bool useFxaa = (s.aaMode == 1 || s.aaMode == 3 || taaReset);
    if (useFxaa) {
        post(doStylize ? ldr0_.get() : ldr1_.get(), true, pFxaa_.get(),
             doStylize ? setFxaaLdr1_.get() : setFxaa_.get(),
             pcWith(glm::mat4(1.0f), glm::vec4(0.0f), glm::vec4(float(intW_), float(intH_), 0, 0)),
             intW_, intH_);
    }
    // The final LDR target: stylize leaves the frame in ldr1, FXAA moves it back to ldr0.
    const bool finalIsLdr1 = doStylize ? !useFxaa : useFxaa;

    // 7. Present to the backbuffer. The SSAA path is downsampled here by bilinear filtering.
    {
        rhi::PassDesc bp;
        bp.backbuffer = true;
        bp.clearColor = true;
        bp.clearValue = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        bp.clearDepth = false;
        dev_.beginPass(bp);
        dev_.setViewport(0, 0, dev_.backbufferWidth(), dev_.backbufferHeight());
        dev_.bindPipeline(pPresent_.get());
        dev_.bindDescriptorSet(0, finalIsLdr1 ? setPresentFx_.get() : setPresent_.get());
        PushConstants pc = pcWith(glm::mat4(1.0f), glm::vec4(dev_.presentFlipsY() ? 1.0f : 0.0f, 0, 0, 0));
        dev_.pushConstants(&pc, sizeof(pc));
        dev_.drawFullscreen();
        dev_.endPass();
    }
    dev_.endGpuTime(3);
}

void Renderer::setHudAtlas(FontAtlas* atlas) {
    hudAtlas_ = atlas;
    hud_.reset();   // the texture is created lazily from the atlas on the next draw
}

uint32_t Renderer::drawHud(const std::vector<HudVertex>& vertices, uint32_t screenW, uint32_t screenH) {
    // One GPU bracket for the whole call (docs/PROFILER.md): it opens before the early-out, so the slot's pair is
    // written on every path and the empty-HUD frame measures zero instead of leaving a dangling timestamp.
    dev_.beginGpuTime(4);
    uint32_t drawn = 0;
    if (hudAtlas_ && hudAtlas_->loaded() && !vertices.empty()) {
        if (!hud_) hud_ = std::make_unique<HudRenderer>(dev_, rhi::Format::Backbuffer);
        hud_->syncAtlas(dev_, *hudAtlas_);
        rhi::PassDesc pass;
        pass.backbuffer = true;
        pass.clearColor = false;   // load the finished frame
        pass.clearDepth = false;
        dev_.beginPass(pass);
        const uint32_t w = dev_.backbufferWidth();
        const uint32_t h = dev_.backbufferHeight();
        dev_.setViewport(0, 0, static_cast<int>(w), static_cast<int>(h));
        drawn = hud_->draw(dev_, vertices, screenW ? screenW : w, screenH ? screenH : h);
        dev_.endPass();
    }
    dev_.endGpuTime(4);
    return drawn;
}

} // namespace ukiyo
