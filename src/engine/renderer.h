// Frame renderer: cascaded shadows, sky, infinite grid, toon materials,
// outlines, dual-filter bloom, tonemapping and anti-aliasing. Written against
// the backend-neutral RHI, so the same pass graph runs on OpenGL and Vulkan.
#pragma once
#include <set>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "engine/camera.h"
#include "engine/gpu_types.h"
#include "engine/hud.h"
#include "engine/graph_material.h"
#include "engine/model.h"
#include "engine/trail.h"
#include "engine/trail_renderer.h"
#include "engine/water.h"
#include "rhi/rhi.h"

namespace ukiyo {

struct RenderSettings {
    int aaMode = 3;                 // 0 off, 1 FXAA, 2 SSAA 2x, 3 SSAA 2x + FXAA, 4 TAA (FXAA on reset frames)
    bool shadows = true;
    bool depthPrepass = true;       // opaque toon draws write depth before the colour pass
    bool frustumCull = true;        // colour passes skip static draws outside the camera frustum
    bool vgeom = true;              // virtual geometry (item 21) for static primitives that have a tree
    float vgeomPixels = 1.0f;       // projected error budget: a cluster refines while its error exceeds this many px
    bool sky = true;
    bool grid = true;
    bool outlines = true;
    bool bloom = true;
    int tonemap = 3;                // 0 ACES fit, 1 PBR Neutral, 2 AgX, 3 Custom Anime (default), 4 Linear clamp,
                                    // 5 Reinhard extended, 6 Hejl-Burgess-Dawson, 7 Hable, 8 Lottes, 9 Uchimura, 10 Genshin rational (Kaze)
    int look = 0;                   // 0 cel, 1 painterly, 2 cinematic (comic-print)
    int stylize = 0;                // 0 off, 1..15 Stylize looks (shaders/stylize.frag, research/stylize.md)
    float stylizeStrength = 1.0f;   // blend with the frame before stylizing: 0 none .. 1 full
    float stylizeOutline = 1.0f;    // ink outline amount: 0 none .. 2 heavy
    float exposure = 0.84f;
    float bloomStrength = 0.12f;
    float bloomThreshold = 1.0f;    // HDR luminance threshold, soft knee applied in bloom_down
    int bloomThresholdMode = 0;     // 0 luminance soft knee (default), 1 Color (per-channel subtract), 2 Brightness (max channel gate)
    glm::vec3 bloomTint{1.0f};      // glow colour multiplier, applied after the threshold; white = off
    float bloomScale = 0.5f;        // bloom size as a fraction of the internal frame, 0.1..1 (0.5 = the original size)
    glm::vec4 bloomWeights{0.25f};  // Kaze's per-level glow weights: blur levels 0 (finest) .. 3 (coarsest)
    float bloomBlurRadius = 2.0f;   // Kaze's blur radius; scaled by bloomScale and frame height / 1080
    bool contrastLogC = false;      // contrast around ACEScc mid grey in LogC (Kaze's Genshin post); false = display pivot at 0.5
    bool saturationLinear = false;  // saturation as a Rec.709 lerp in linear light (Kaze's); false = display vibrance
    float saturation = 1.18f;
    float contrast = 1.12f;
    float vignette = 0.12f;
    glm::vec3 shadowTint{0.0f};     // display-space offset added to shadows, about -0.2..0.2 per channel
    glm::vec3 highlightTint{0.0f};  // display-space offset added to highlights
    float whiteBalance = 0.0f;      // -1 cool .. +1 warm
    float lutAmount = 1.0f;         // blend with the loaded grading LUT (no effect while none is loaded)
    float animeKnee = 0.45f;        // Custom Anime: where the highlight shoulder starts (0.3 .. 0.95). 0.45: at HDR 2.0 output is 0.967
    float animeToe = 0.03f;         // Custom Anime: shadow lift (0 .. 0.05)
    bool dither = true;             // interleaved gradient noise at the 8-bit output step
    float grain = 0.0f;             // film grain: off, it breaks flat cel fills
    float outlineScale = 1.0f;
    // RealToon extras for toon materials. All zero or false = off, so existing renders are unchanged.
    float hairHighlight = 0.0f;     // strength of the anisotropic hair highlight, 0..1
    float hairShift = 0.1f;         // moves the highlight band along the normal
    float hairExponent = 60.0f;     // sharpness of the highlight band
    bool selfShadow = false;        // second shadow threshold on the lit ramp
    float selfThreshold = 0.5f;     // where the self shadow starts, on N.L remapped to 0..1
    float selfHardness = 0.08f;     // width of the self shadow edge
    float gloss = 0.0f;             // strength of the toon gloss, 0..1
    float glossPower = 32.0f;       // gloss sharpness
    float sunIntensity = 1.25f;
    // Screen-space and grading effects. Each is off at its default, so existing renders are unchanged.
    float fogDensity = 0.0f;         // exponential view-distance fog per metre (0 = off). No height term.
    glm::vec3 fogColor{0.62f, 0.72f, 0.86f};
    float fogSun = 0.0f;             // sun in-scatter into the fog, 0..1 (needs fogDensity > 0)
    float ssao = 0.0f;               // ambient occlusion strength, 0..1 (normal pre-pass)
    // Multi-bounce ambient occlusion (GH-115426): the AO term restored by the light that bounces between the surfaces
    // doing the occluding, weighted by each surface's albedo. Off leaves the plain single-bounce multiply; it has no
    // effect while `ssao` is 0. Needs the albedo the normal pre-pass writes, and falls back to single-bounce on
    // geometry that pre-pass does not cover (terrain, foliage, grass, graph materials).
    bool multiBounceAO = true;
    // Screen-space contact shadows (GH-118045): the short shadows a cascaded shadow map cannot resolve, marched in the
    // depth buffer towards the sun. 0 is off. Length and thickness are metres; steps is the depth fetch count.
    float contactShadows = 0.0f;
    float contactShadowLength = 0.35f;
    float contactShadowThickness = 0.08f;
    int contactShadowSteps = 24;
    // Alpha-coverage-preserving mips (GH-104289): the base-colour chain of an alpha-masked material keeps the level-0
    // test coverage, so a cutout does not dissolve into its average at distance. It is a load-time option, so a change
    // takes effect when the model is loaded again.
    bool preserveAlphaCoverage = true;
    float alphaTestThreshold = 0.5f;
    float ssr = 0.0f;                // screen-space reflections, 0..1 (normal pre-pass)
    // Real-time diffuse GI (the iGPU path tracer): half-resolution traced bounce light, temporally accumulated and
    // composited at full resolution. 0 = off, 1 = one bounce at full strength. Needs a heightfield (setHeightfield) and
    // a normal pre-pass (it is turned on automatically when this is above zero, as SSAO is).
    float gi = 0.0f;                 // indirect diffuse strength, 0..1
    float giRadius = 12.0f;          // how far a bounce ray marches, metres
    float giRays = 8.0f;             // rays per half-resolution pixel per frame, 1..8
    float giHistory = 0.0625f;       // weight of one frame's samples in the accumulation (1/16 is 16 frames)
    // Two à-trous iterations over the accumulated GI before it is composited. ~50 taps at half resolution; it halves
    // the variance at one ray per pixel, which is what lets the pass stay cheap on an iGPU. See shaders/gi_denoise.frag.
    bool giDenoise = true;
    float depthNormalOutline = 0.0f; // depth and normal edge lines, 0..1 (normal pre-pass)
    float flare = 0.0f;              // lens flare around the sun, 0..1 (occluded by geometry)
    float motionBlur = 0.0f;         // motion blur shutter, 0..2. Runs BEFORE TAA, so it also smears TAA history.
    float chromatic = 0.0f;          // chromatic aberration at the screen edges, 0..1
    float sharpen = 0.0f;            // unsharp mask, 0..1
    bool autoExposure = false;       // adapt exposure to the frame's average brightness
    float autoKey = 0.25f;           // auto exposure target average luminance
    bool celBand = false;            // three-step cel ramp in the mtoon shader
    float screenRim = 0.0f;          // RealToon screen-space rim light, 0..1 (normal pre-pass, scene-wide)
    float outlineNoise = 0.0f;       // RealToon outline noise (sketch), 0..1: dashes the depth-normal outline
    float depthEdgeRim = 0.0f;       // depth-edge rim, 0..1: light on the silhouette of a surface in front of farther geometry
    float normalMap = 0.0f;          // RealToon normal map strength, 0..1 (uses a material's normal texture; 0 = off)
    float outlineMatNoise = 0.0f;    // RealToon material outline noise, 0..1: dashes the inverted-hull outline
    float reflection = 0.0f;         // RealToon reflection strength (procedural sky on MToon materials; 0 = off)
    float smear = 0.0f;              // RealToon smear: how far the cel edge is pulled along the motion, 0..1 (0 = off)
    float softLight = 0.0f;          // soft light: a wrap term that lifts the shade side of toon materials, 0..1
    float lightSweep = 0.0f;         // light sweep: a bright band that climbs the character, 0..1 (0 = off)
    float sweepSpeed = 0.25f;        // light sweep: cycles per second
    float ditherFadeNear = 0.0f;     // camera dither fade: fully dithered out at this distance (m)
    float ditherFadeFar = 0.0f;      // camera dither fade: fully visible at this distance (m); off unless above ditherFadeNear
    float pbrStyle = 0.0f;           // stylised PBR: banded GGX highlight and metal tint on toon materials, 0..1
    float eyeFlatten = 0.0f;         // eye: blend toward flat full light (0 = lit as normal, 1 = flat), 0..1
    float eyeParallax = 0.0f;        // eye: parallax pupil strength in UV units (0 = off)
    float eyeMatcap = 0.0f;          // eye: matcap strength (needs a matcap texture on the eye material), 0..1
    float eyeHighlight = 0.0f;       // eye: highlight map strength (needs a highlight texture on the eye material), 0..1
    glm::vec4 occlusionTarget{0.0f}; // occlusion dither: xyz world point the camera looks at, w radius (0 = off)
    bool smoothNormals = false;      // RealToon smooth object normals: applied when a model loads (reload to change)
    bool faceShadow = false;         // RealToon face shadow: use the face SDF on materials loaded with one
    // SplitScreen line (shaders/SplitScreen.shader). Off while splitDir is zero. The NPR side (screen side > 0)
    // is multiplied by splitNprTint, the other side by splitPbrTint, and a line of splitLineWidthPx is drawn on the
    // split. The direction is normalised here. The offset and the frame's pixels use the materials' convention
    // (origin at the centre, y up), so the line sits on the same split as the materials' own split.
    glm::vec2 splitDir{0.0f};
    float splitOffsetPx = 0.0f;
    float splitLineWidthPx = 2.0f;
    glm::vec3 splitLineColor{1.0f};
    glm::vec3 splitNprTint{1.0f};    // 0..1 per channel
    glm::vec3 splitPbrTint{1.0f};    // 0..1 per channel
    glm::vec3 sunDir = glm::normalize(glm::vec3(0.35f, 0.75f, 0.55f));   // towards the sun
};

// A terrain chunk to draw (item 18). The buffers belong to the terrain instance. The model moves the chunk from tile
// space to the world, and the bounds are in tile space.
// Everything the real-time GI pass needs to know about the scene's heightfield. A heightmap is a height function, so
// the estimator needs no acceleration structure: it marches the same samples the terrain is built from. Set once per
// terrain tile; a scene without terrain simply leaves it empty and the pass does not run.
struct HeightfieldGpu {
    rhi::Texture* height = nullptr;   // R = height normalised to 0..1, one texel per grid sample
    uint32_t size = 0;                // samples per side (the texture is size x size)
    float cellSize = 1.0f;            // metres between samples
    float heightScale = 1.0f;         // metres for a normalised height of 1
    glm::vec3 origin{0.0f};           // world position of sample (0, 0)
};

struct TerrainDraw {
    rhi::Buffer* vertices = nullptr;
    rhi::Buffer* indices = nullptr;
    uint32_t indexCount = 0;
    glm::mat4 model{1.0f};
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    rhi::DescriptorSet* layers = nullptr;   // ShaderMaterial set with the layer colours (binding 15)
};

// One foliage batch to draw (item 18, step 3): up to kFoliageBatch instances of one mesh, drawn as one instanced call.
// The bounds are the chunk's, in tile space, and the fade is in metres from the camera.
struct FoliageDraw {
    rhi::Buffer* vertices = nullptr;
    rhi::Buffer* indices = nullptr;
    uint32_t indexCount = 0;
    uint32_t instanceCount = 0;
    rhi::DescriptorSet* instances = nullptr;   // ShaderMaterial set with the instance rows (binding 15)
    glm::mat4 model{1.0f};
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    glm::vec2 fade{60.0f, 90.0f};
    glm::vec3 tint{0.3f, 0.5f, 0.2f};
};

// One grass batch to draw (item 19): up to kGrassBatch blades of one tuft mesh, drawn as one instanced call. The
// bands are metres from the camera: full density inside x, half at y, nothing past z. `wind` is the strength (m), the
// frequency (cycles/s), the spatial scale (rad/m) and the flutter (m). `windDir` holds the ground direction in xy.
// `pusher` is a body standing in the grass: xy its ground position, z the reach in metres, w the push in metres (w = 0
// turns it off). All of it goes in the push block, so the shader needs no extra buffer.
struct GrassDraw {
    rhi::Buffer* vertices = nullptr;
    rhi::Buffer* indices = nullptr;
    rhi::DescriptorSet* instances = nullptr;   // ShaderMaterial set with the rows and colours (binding 15)
    uint32_t indexCount = 0;
    uint32_t instanceCount = 0;
    glm::mat4 model{1.0f};
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    glm::vec3 bands{24.0f, 48.0f, 95.0f};
    glm::vec4 wind{0.22f, 0.45f, 0.11f, 0.05f};
    glm::vec2 windDir{0.85f, 0.53f};
    glm::vec4 pusher{0.0f};
};

// Kaze's Unity Volume values (Bloom Volume and Color Grading Volume), as set in the Unity screenshot.
// Only the fields that the Unity volumes control change; the rest of the settings are left alone.
inline void applyKazeUnityPost(RenderSettings& s) {
    s.bloom = true;
    s.bloomThresholdMode = 1;               // Mode: Color
    s.bloomThreshold = 0.7f;                // Threshold
    s.bloomStrength = 1.5f;                 // Intensity
    s.bloomWeights = glm::vec4(0.1f, 0.2f, 0.3f, 0.4f);   // Weights
    s.bloomTint = glm::vec3(1.0f);          // Color
    s.bloomBlurRadius = 2.0f;               // Blur Radius
    s.bloomScale = 0.5f;                    // Down Sample Scale
    s.tonemap = 10;                         // Use Tonemapping: the Genshin rational curve
    s.exposure = 1.05f;                     // Exposure
    s.contrast = 1.0f;                      // Contrast 0 in Unity: neutral
    s.contrastLogC = false;
    s.saturation = 1.0f;                    // Saturation 0 in Unity: neutral, with the linear mode
    s.saturationLinear = true;
}

class Renderer {
public:
    explicit Renderer(rhi::Device& device);
    ~Renderer();

    // Renders one frame into the backbuffer. Call between device.beginFrame() and endFrame().
    // Joint and material buffers must be updated (updateModel) before this call.
    void render(const OrbitCamera& cam, const std::vector<Model*>& models,
                float time, uint32_t frameIndex, const RenderSettings& settings);

    // ---- HUD ------------------------------------------------------------------------------------------------
    // The glyph atlas is borrowed and must outlive the renderer. drawHud() is called after render() and before
    // device.endFrame(); it opens and closes its own pass on the presentation target, so the scene render is untouched.
    void setHudAtlas(FontAtlas* atlas);
    // Draws the canvas vertices (pixels, origin top-left). Returns the vertices drawn; 0 when there is no atlas.
    uint32_t drawHud(const std::vector<HudVertex>& vertices, uint32_t screenW, uint32_t screenH);

    // Trails to draw this frame. The renderer keeps the pointers only for the duration of render(): the caller owns
    // the Trail objects, and they must outlive the call. Alpha trails are drawn after the blended materials so they
    // composite over them; additive trails (sword arcs, light streaks) are drawn after those. Each list is one draw.
    void setTrails(const std::vector<const Trail*>& alpha, const std::vector<const Trail*>& additive = {});
    // Triangles the trail draw produced in the last render().
    uint32_t lastTrailTriangles() const { return trailTriangles_; }

    // Mesh draw calls issued by the last render(), across the shadow and scene passes.
    uint32_t lastDrawCalls() const { return drawCalls_; }
    uint32_t lastTriangles() const { return triangles_; }

    // Totals of the virtual-geometry cuts collected by the last render(): clusters chosen, source triangles those
    // clusters hold, and subtrees rejected by the frustum. Zero when no primitive in the scene has a tree.
    VGeomStats lastVGeomStats() const { return vgeomStats_; }

    // Draws a primitive with a Shader Graph material instead of its MToon material. Pass nullptr to
    // restore the MToon path. The renderer does not own the GraphMaterial; it must outlive the draws.
    // Graph primitives are drawn opaque in the scene pass and are not shadowed. A graph material whose
    // shader has an outline gets the inverted-hull outline too, when outlines are on.
    void setGraphMaterial(const Primitive* prim, GraphMaterial* material);

    // Draws a primitive with the MMD-style toon look (shaders/mmdtoon.frag and mmdedge.frag)
    // instead of MToon. Applies to opaque and alpha-blended MToon primitives; graph materials take
    // precedence. Edges follow the global outline toggle. Pass false to restore MToon.
    void setMmdToon(const Primitive* prim, bool on);
    void clearMmdToon() { mmdToon_.clear(); }

    // Loads a colour grading LUT: a 256x16 PNG holding a 16-cube as 16 slices of 16x16 (red across,
    // green down, blue by slice). An empty path restores the identity LUT and turns grading off.
    // Returns false with `error` set when the file cannot be read or has the wrong size.
    bool setGradingLut(const std::string& pngPath, std::string* error);

    // Sets the water volume drawn by render() (item 16). Pass nullptr to remove it. The renderer keeps a copy.
    void setWater(const WaterVolume* volume);
    // The terrain chunks to draw this frame (item 18). The buffers belong to the terrain instances, so pass the list
    // again every frame.
    void setTerrain(const std::vector<TerrainDraw>& chunks) { terrain_ = chunks; }
    void setHeightfield(const HeightfieldGpu& field) { heightfield_ = field; }
    void setFoliage(const std::vector<FoliageDraw>& batches) { foliage_ = batches; }
    void setGrass(const std::vector<GrassDraw>& batches) { grass_ = batches; }
    bool hasWater() const { return water_.has_value(); }
    bool hasGradingLut() const { return hasLut_; }
    // Removes every override. Call before a GraphMaterial is destroyed or its model is replaced.
    void clearGraphMaterials() { graphMaterials_.clear(); }

private:
    // One frame's selection for one draw: the clusters the walk chose, and the contiguous runs of the cluster index
    // stream they form, so the renderer issues one draw per run instead of one per cluster.
    struct VGeomCut {
        std::vector<uint32_t> clusters;
        std::vector<std::pair<uint32_t, uint32_t>> runs;   // (firstIndex, indexCount)
    };
    struct Rec {
        Model* model;
        int node;
        int skin;
        const Primitive* prim;
        float fade = 0.0f;     // distance fade of the model (colour passes)
        bool inView = true;    // false when the camera frustum culls the draw (colour passes skip it)
        int lod = 0;           // mesh LOD level for this draw (0 = full detail); static draws only
        const VGeomCut* vgeomCut = nullptr;   // null when the primitive has no tree or the setting is off
    };

    void rebuildTargets(uint32_t scale);
    void buildPipelines();
    void buildPostSets();
    std::unique_ptr<rhi::Texture> makeLutTexture(const uint8_t* rgba, uint32_t size = 16) const;   // size N: N*N x N strip

    rhi::Device& dev_;
    // Trails for the current frame, and the two pipelines that draw them (created on first use).
    std::vector<const Trail*> trailAlpha_;
    std::vector<const Trail*> trailAdditive_;
    std::unique_ptr<TrailRenderer> trailAlphaRenderer_;
    std::unique_ptr<TrailRenderer> trailAdditiveRenderer_;
    uint32_t trailTriangles_ = 0;

    uint32_t drawCalls_ = 0;
    uint32_t triangles_ = 0;     // triangles in the mesh draws of the last frame
    VGeomStats vgeomStats_{};
    float bloomScale_ = 0.5f;     // bloom size the targets were built with
    uint32_t scale_ = 0;          // internal resolution scale (1 or 2); 0 = not built
    uint32_t displayW_ = 0, displayH_ = 0;
    uint32_t intW_ = 0, intH_ = 0;

    // Fixed-size resources.
    std::unique_ptr<rhi::Buffer> frameUbo_;
    std::unique_ptr<class HudRenderer> hud_;
    FontAtlas* hudAtlas_ = nullptr;
    std::unique_ptr<rhi::Texture> shadowAtlas_;     // 4096, 2x2 cascades of 2048
    std::unique_ptr<rhi::Texture> dummyTex_;        // used by the shadow pass's Frame set
    std::unique_ptr<rhi::DescriptorSet> frameSet_;
    std::unique_ptr<rhi::DescriptorSet> shadowFrameSet_;

    // Pipelines.
    std::unique_ptr<rhi::Pipeline> pShadow_, pSky_, pGrid_, pGridWater_, pOutline_;
    std::unique_ptr<rhi::Pipeline> pMToon_[2][2];   // [blend][doubleSided]
    std::unique_ptr<rhi::Pipeline> pDepth_[2];      // depth prepass, [doubleSided]
    std::unique_ptr<rhi::Pipeline> pMmdToon_[2][2];  // MMD-style toon look [blend][doubleSided]
    std::unique_ptr<rhi::Pipeline> pMmdEdge_;        // MMD-style edge (inverted hull, front culled)
    std::unique_ptr<rhi::Pipeline> pBloomDown_, pBloomBlur_, pBloomCombine_, pTonemap_, pFxaa_, pPresent_, pTaa_, pStylize_;
    std::unique_ptr<rhi::Pipeline> pVelocity_;      // per-object motion vectors (TAA or motion blur)
    std::unique_ptr<rhi::Pipeline> pNormal_;        // view-space normals for SSAO, SSR and the outline
    std::unique_ptr<rhi::Pipeline> pEffects_;       // screen-space effects (effects.frag)
    std::unique_ptr<rhi::Pipeline> pExpo_;          // auto exposure reduce and copy (expo_reduce.frag)
    std::unique_ptr<rhi::Pipeline> pWaterCopy_;     // scene colour + view depth into the refraction target
    std::unique_ptr<rhi::Pipeline> pWater_;         // water surface (water.vert / water.frag)
    std::unique_ptr<rhi::Pipeline> pUnderwater_;    // underwater fog (underwater.frag)
    std::unique_ptr<rhi::Pipeline> pTerrain_;       // terrain chunks (terrain.vert / terrain.frag)
    std::unique_ptr<rhi::Pipeline> pTerrainShadow_;  // the same chunks as shadow casters (terrain_shadow.vert)
    std::vector<TerrainDraw> terrain_;
    HeightfieldGpu heightfield_;
    // Real-time GI (the iGPU path tracer): a half-resolution trace, a temporal accumulator and a composite that the
    // effects pass folds in. The two GI targets and their sets are built with the rest of the resolution-dependent
    // targets; the pass does nothing when no heightfield is set or gi_ is zero.
    std::unique_ptr<rhi::Texture> gi_, giDenoise_, giHist_[2];
    std::unique_ptr<rhi::Pipeline> pGiTrace_, pGiAccum_, pGiDenoise_;
    std::unique_ptr<rhi::DescriptorSet> setGiTrace_, setGiAccum_[2], setGiDenoise_[2], setGiDenoiseB_;
    uint32_t giFrame_ = 0;
    uint32_t giWrite_ = 0;
    std::unique_ptr<rhi::Pipeline> pFoliage_;       // foliage batches (foliage.vert / foliage.frag)
    std::vector<FoliageDraw> foliage_;
    std::unique_ptr<rhi::Pipeline> pGrass_;         // grass batches (grass.vert / grass.frag)
    std::vector<GrassDraw> grass_;

    // Water (item 16). The grid is a fixed 128 x 128 rest grid over [0,1]^2, displaced in water.vert.
    std::optional<WaterVolume> water_;
    std::unique_ptr<rhi::Buffer> waterVb_, waterIb_;
    std::unique_ptr<rhi::Buffer> contactUbo_;   // 16 bytes: the effects pass's contact-shadow parameters (Post binding 7)
    uint32_t waterIndexCount_ = 0;

    // Resolution-dependent targets.
    std::unique_ptr<rhi::Texture> hdr_, depth_, ldr0_, ldr1_;
    std::unique_ptr<rhi::Texture> velocity_;       // RGBA16F motion vectors in UV units (TAA, motion blur)
    std::unique_ptr<rhi::Texture> refr_;           // half-size RGBA16F: scene colour (rgb) and view depth (a), for water
    std::unique_ptr<rhi::Texture> normal_;         // RGBA8 view-space normals; alpha 1 = opaque surface
    std::unique_ptr<rhi::Texture> albedo_;         // RGBA8 linear base colour for multi-bounce AO; alpha 1 = present
    std::unique_ptr<rhi::Texture> noAlbedo_;       // 1x1 transparent, bound at Post 6 when multi-bounce AO is off
    std::unique_ptr<rhi::Texture> fxHdr_;          // HDR after the effects pass: input to TAA, bloom, tonemap
    std::array<std::unique_ptr<rhi::Texture>, 2> expo_;   // 1x1 auto-exposure state; expo_[0] is read by tonemap
    std::unique_ptr<rhi::Texture> lut_;             // 256x16 grading strip; identity until a LUT is loaded
    std::unique_ptr<rhi::Texture> blueNoise_;       // 64x64 dither tile (blue_noise_64.h), bound at Post binding 6
    bool hasLut_ = false;
    std::array<std::unique_ptr<rhi::Texture>, 4> bloom_;      // blurred level i (level 0 is bloomScale of the frame)
    std::array<std::unique_ptr<rhi::Texture>, 4> bloomTmp_;   // horizontal-blur result for level i
    std::unique_ptr<rhi::Texture> bloomOut_;                  // weighted sum of the levels; read by tonemap

    // Post-processing descriptor sets (rebuilt with the targets).
    std::array<std::unique_ptr<rhi::DescriptorSet>, 4> setDown_;
    std::array<std::unique_ptr<rhi::DescriptorSet>, 4> setBlurH_;  // horizontal blur of level i (reads bloom_[i], or bloom_[i-1] for i > 0)
    std::array<std::unique_ptr<rhi::DescriptorSet>, 4> setBlurV_;  // vertical blur of level i (reads bloomTmp_[i])
    std::unique_ptr<rhi::DescriptorSet> setCombine_;               // the four levels for the weighted sum
    std::unique_ptr<rhi::DescriptorSet> setTonemap_, setFxaa_, setFxaaLdr1_, setStylize_;
    std::unique_ptr<rhi::DescriptorSet> setEffects_, setExpoReduce_, setExpoCopy_;
    // TAA: two history targets ping-pong. taaWrite_ is written this frame; the other one is the
    // previous resolved frame. Bloom and tonemap read the resolved target through setDownTaa_ and
    // setTonemapTaa_ (indexed by the resolved target).
    std::array<std::unique_ptr<rhi::Texture>, 2> taaHist_;
    std::array<std::unique_ptr<rhi::DescriptorSet>, 2> setTaa_, setDownTaa_, setTonemapTaa_;
    // Per-draw motion state. Key: model and node (rigid) or model and skin (skinned). Slots are
    // reused across frames; `last` holds the previous frame's transforms.
    struct MotionSlot {
        std::unique_ptr<rhi::Buffer> ubo;              // MotionUBO
        std::unique_ptr<rhi::DescriptorSet> set;       // SetType::Motion
        std::vector<glm::mat4> last;                   // previous frame (empty until the first frame)
        std::vector<glm::mat4> current;                // this frame, copied to `last` after the pass
    };
    std::map<std::pair<const void*, int>, MotionSlot> motion_;
    std::map<const Model*, glm::vec3> hipsPrev_;     // hips world position last frame, per model (smear)
    std::map<const Model*, glm::vec3> motionDisp_;   // this frame's hips displacement per model (smear)
    glm::mat4 prevViewProj_{1.0f};                 // previous frame's unjittered viewProj
    bool prevViewReady_ = false;                   // false until the first frame has set prevViewProj_
    bool taaHistoryValid_ = false;
    uint32_t taaWrite_ = 0;
    std::unique_ptr<rhi::DescriptorSet> setPresent_, setPresentFx_;
    std::unique_ptr<rhi::DescriptorSet> waterCopySet_;   // Post set: hdr_ at 1, depth_ at 4
    std::unique_ptr<rhi::DescriptorSet> waterSet_;       // Post set: refr_ at 2 (water and underwater passes)

    std::map<const Primitive*, GraphMaterial*> graphMaterials_;
    std::set<const Primitive*> mmdToon_;
};

} // namespace ukiyo
