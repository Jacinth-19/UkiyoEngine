// Ukiyo Engine application.
//
//   Interactive:  ukiyo --backend gl|vk [--model assets/Seed-san.vrm]
//                 Mouse: LMB orbit, RMB/MMB pan, wheel zoom.
//                 Keys:  WASD move target, Q/E up/down, 1-4 AA mode, G grid, B bloom,
//                        O outlines, S shadows, K sky, SPACE auto-orbit, R reset, P screenshot.
//
//   Headless:     ukiyo --headless --backend gl --shot out/shot.png
//                 ukiyo --headless --backend vk --video out/orbit.mp4 --seconds 6 --fps 30
//
// Headless rendering uses an offscreen backbuffer on both backends, so it runs
// unchanged under Mesa (llvmpipe) with Xvfb for the GL device.
#include <glm/gtc/matrix_transform.hpp>
#include <GLFW/glfw3.h>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/anim.h"
#include "engine/camera.h"
#include "engine/face.h"
#include "engine/model.h"
#include "engine/rig.h"
#include "engine/renderer.h"
#include "input/input.h"
#include "input/input_asset.h"
#include "input/input_glfw.h"
#include "rhi/rhi.h"
#include "stb_image_write.h"

using namespace ukiyo;

namespace {

struct Options {
    rhi::Backend backend = rhi::Backend::Vulkan;
    std::string model = "assets/Seed-san.vrm";
    bool headless = false;
    uint32_t width = 1280, height = 720;
    int frames = 1;                 // headless still frames when no video is requested
    std::string faceSdf;            // face shadow SDF atlas PNG (--face-sdf)
    std::string faceMaterial = "face"; // material name part that uses it (--face-material)
    std::string shot;               // headless PNG output (last frame)
    std::string lut;                // colour grading LUT (256x16 PNG), empty for none
    std::string video;              // headless MP4 output
    int fps = 30;
    float seconds = 6.0f;
    float orbitTurns = 0.25f;       // camera turn over the whole run (headless)
    float yaw = 0.0f, pitch = 0.08f, distance = -1.0f, targetY = -1.0f;
    RenderSettings settings;
    bool physics = true;            // spring bones
    float hitAt = -1.0f;            // headless: trigger a hit at this time (s)
    float hitStrength = 6.0f;       // angular impulse (rad/s) on the upper body
    float swayAmp = 0.0f;           // headless: root sways sideways by this many metres
    std::string face;               // face timeline JSON (tools/face_align.py)
    std::string audio;              // audio clip muxed into --video output
    // Animation.
    std::string animLib = "assets/anim/UAL1_Standard.glb";  // CC0 Universal Animation Library
    std::string clip;               // play one clip on loop (--anim)
    bool idle = false;              // idle state machine (--idle)
    uint32_t idleSeed = 1;
    int handOnHip = -1;             // force hand-on-waist: 0 right, 1 left (--hand-on-hip)
    bool armIK = true;              // torso collision and hand IK (--no-arm-ik disables)
    bool ikDebug = false;           // print arm clearance (--ik-debug)
    // Robotic arm rig (Seed-san's backpack arm).
    bool roboOn = false;
    float roboTarget[3] = {0.0f, 1.2f, 0.4f};   // world-space target (--robo-target X,Y,Z)
    bool roboTrack = false;         // moving target (--robo-track)
    bool roboFabrik = false;        // FABRIK chain instead of two-bone (--robo-solver fabrik)
    float roboWeight = 1.0f;        // rig layer weight (--robo-weight)
};

void usage() {
    std::printf(
        "usage: ukiyo [--backend gl|vk] [--model file.vrm] [--headless] [--size WxH]\n"
        "             [--shot out.png] [--video out.mp4] [--fps N] [--seconds S]\n"
        "             [--turns T] [--yaw R] [--pitch R] [--distance D] [--target-y Y]\n"
        "             [--aa 0-4 (4 = TAA)] [--no-shadows] [--no-grid] [--no-bloom] [--no-outlines]\n"
        "             [--no-sky] [--no-physics] [--hit SECONDS] [--hit-strength V] [--sway METRES] [--face FACE.json] [--audio CLIP] [--frames N]\n"
             "             [--tonemap 0-10 (0 ACES, 1 Neutral, 2 AgX, 3 Custom Anime, 4 Linear, 5 Reinhard, 6 Hejl, 7 Hable, 8 Lottes, 9 Uchimura, 10 Genshin)] [--exposure E] [--bloom B] [--bloom-threshold T] [--saturation S]\n"
             "             [--bloom-mode 0-2 (0 knee, 1 Color, 2 Brightness)] [--bloom-scale 0.1..1] [--bloom-tint R,G,B] [--contrast-log] [--saturation-linear]\n"
             "             [--bloom-weights A,B,C,D] [--bloom-radius R] [--post-preset kaze-unity]\n"
             "             [--contrast C] [--vignette V] [--grain G] [--look cel|painterly|cinematic]\n"
             "             [--stylize 0-15] [--stylize-strength 0..1] [--stylize-outline 0..2]\n"
             "             [--fog D] [--fog-sun 0..1] [--ssao 0..1] [--ssr 0..1] [--outline-dn 0..1] [--flare 0..1]\n"
             "             [--motion-blur 0..2] [--chromatic 0..1] [--sharpen 0..1] [--auto-exposure] [--auto-key K] [--cel-band] [--sun-dir X,Y,Z]\n"
             "             [--screen-rim 0..1] [--outline-noise 0..1] [--normal-map 0..1] [--outline-mat-noise 0..1] [--smooth-normals]\n"
             "             [--face-sdf PNG] [--face-material NAME]\n"
             "             [--hair-highlight 0..1] [--hair-shift S] [--hair-exponent E] [--self-shadow]\n"
             "             [--self-threshold T] [--self-hardness H] [--gloss 0..1] [--gloss-power P]\n"
             "             [--white-balance -1..1] [--shadow-tint R,G,B] [--highlight-tint R,G,B] [--lut PNG] [--lut-amount 0..1]\n"
        "             [--anim-lib FILE.glb] [--anim CLIP] [--idle] [--idle-seed N] [--hand-on-hip right|left]\n"
        "             [--no-arm-ik] [--ik-debug]\n"
        "             [--robo-target X,Y,Z] [--robo-track] [--robo-solver two-bone|fabrik] [--robo-weight W]\n");
}

Options parseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { usage(); std::exit(2); }
            return argv[++i];
        };
        if (a == "--backend") {
            std::string b = next();
            o.backend = (b == "gl" || b == "opengl") ? rhi::Backend::OpenGL : rhi::Backend::Vulkan;
        } else if (a == "--model") o.model = next();
        else if (a == "--headless") o.headless = true;
        else if (a == "--size") {
            std::string s = next();
            std::sscanf(s.c_str(), "%ux%u", &o.width, &o.height);
        } else if (a == "--shot") o.shot = next();
        else if (a == "--video") o.video = next();
        else if (a == "--fps") o.fps = std::atoi(next().c_str());
        else if (a == "--seconds") o.seconds = float(std::atof(next().c_str()));
        else if (a == "--turns") o.orbitTurns = float(std::atof(next().c_str()));
        else if (a == "--frames") o.frames = std::atoi(next().c_str());
        else if (a == "--yaw") o.yaw = float(std::atof(next().c_str()));
        else if (a == "--pitch") o.pitch = float(std::atof(next().c_str()));
        else if (a == "--distance") o.distance = float(std::atof(next().c_str()));
        else if (a == "--target-y") o.targetY = float(std::atof(next().c_str()));
        else if (a == "--aa") o.settings.aaMode = std::atoi(next().c_str());
        else if (a == "--no-shadows") o.settings.shadows = false;
        else if (a == "--no-grid") o.settings.grid = false;
        else if (a == "--no-bloom") o.settings.bloom = false;
        else if (a == "--no-outlines") o.settings.outlines = false;
        else if (a == "--no-sky") o.settings.sky = false;
        else if (a == "--no-physics") o.physics = false;
        else if (a == "--hit") o.hitAt = float(std::atof(next().c_str()));
        else if (a == "--hit-strength") o.hitStrength = float(std::atof(next().c_str()));
        else if (a == "--sway") o.swayAmp = float(std::atof(next().c_str()));
        else if (a == "--face") o.face = next();
        else if (a == "--tonemap") o.settings.tonemap = std::atoi(next().c_str());
        else if (a == "--look") {
            std::string v = next();
            o.settings.look = v == "painterly" ? 1 : v == "cinematic" ? 2 : std::atoi(v.c_str());
        }
        else if (a == "--stylize") o.settings.stylize = std::atoi(next().c_str());
        else if (a == "--hair-highlight") o.settings.hairHighlight = float(std::atof(next().c_str()));
        else if (a == "--hair-shift") o.settings.hairShift = float(std::atof(next().c_str()));
        else if (a == "--hair-exponent") o.settings.hairExponent = float(std::atof(next().c_str()));
        else if (a == "--self-shadow") o.settings.selfShadow = true;
        else if (a == "--self-threshold") o.settings.selfThreshold = float(std::atof(next().c_str()));
        else if (a == "--self-hardness") o.settings.selfHardness = float(std::atof(next().c_str()));
        else if (a == "--gloss") o.settings.gloss = float(std::atof(next().c_str()));
        else if (a == "--gloss-power") o.settings.glossPower = float(std::atof(next().c_str()));
        else if (a == "--stylize-strength") o.settings.stylizeStrength = float(std::atof(next().c_str()));
        else if (a == "--stylize-outline") o.settings.stylizeOutline = float(std::atof(next().c_str()));
        else if (a == "--fog") o.settings.fogDensity = float(std::atof(next().c_str()));
        else if (a == "--fog-sun") o.settings.fogSun = float(std::atof(next().c_str()));
        else if (a == "--ssao") o.settings.ssao = float(std::atof(next().c_str()));
        else if (a == "--ssr") o.settings.ssr = float(std::atof(next().c_str()));
        else if (a == "--outline-dn") o.settings.depthNormalOutline = float(std::atof(next().c_str()));
        else if (a == "--flare") o.settings.flare = float(std::atof(next().c_str()));
        else if (a == "--motion-blur") o.settings.motionBlur = float(std::atof(next().c_str()));
        else if (a == "--chromatic") o.settings.chromatic = float(std::atof(next().c_str()));
        else if (a == "--sharpen") o.settings.sharpen = float(std::atof(next().c_str()));
        else if (a == "--auto-exposure") o.settings.autoExposure = true;
        else if (a == "--auto-key") o.settings.autoKey = float(std::atof(next().c_str()));
        else if (a == "--cel-band") o.settings.celBand = true;
        else if (a == "--screen-rim") o.settings.screenRim = float(std::atof(next().c_str()));
        else if (a == "--outline-noise") o.settings.outlineNoise = float(std::atof(next().c_str()));
        else if (a == "--depth-edge-rim") o.settings.depthEdgeRim = float(std::atof(next().c_str()));
        else if (a == "--normal-map") o.settings.normalMap = float(std::atof(next().c_str()));
        else if (a == "--outline-mat-noise") o.settings.outlineMatNoise = float(std::atof(next().c_str()));
        else if (a == "--reflect") o.settings.reflection = float(std::atof(next().c_str()));
        else if (a == "--smear") o.settings.smear = float(std::atof(next().c_str()));
        else if (a == "--soft-light") o.settings.softLight = float(std::atof(next().c_str()));
        else if (a == "--light-sweep") o.settings.lightSweep = float(std::atof(next().c_str()));
        else if (a == "--sweep-speed") o.settings.sweepSpeed = float(std::atof(next().c_str()));
        else if (a == "--pbr-style") o.settings.pbrStyle = float(std::atof(next().c_str()));
        else if (a == "--eye-flatten") o.settings.eyeFlatten = float(std::atof(next().c_str()));
        else if (a == "--eye-parallax") o.settings.eyeParallax = float(std::atof(next().c_str()));
        else if (a == "--eye-matcap") o.settings.eyeMatcap = float(std::atof(next().c_str()));
        else if (a == "--eye-highlight") o.settings.eyeHighlight = float(std::atof(next().c_str()));
        else if (a == "--dither-fade") {                 // NEAR FAR, metres
            o.settings.ditherFadeNear = float(std::atof(next().c_str()));
            o.settings.ditherFadeFar = float(std::atof(next().c_str()));
        }
        else if (a == "--smooth-normals") o.settings.smoothNormals = true;
        else if (a == "--face-sdf") { o.faceSdf = next(); o.settings.faceShadow = true; }
        else if (a == "--face-material") o.faceMaterial = next();
        else if (a == "--sun-dir") {   // towards the sun, X,Y,Z (normalised by the renderer)
            float x = 0, y = 0, z = 0;
            if (std::sscanf(next().c_str(), "%f,%f,%f", &x, &y, &z) == 3) o.settings.sunDir = glm::vec3(x, y, z);
        }
        else if (a == "--exposure") o.settings.exposure = float(std::atof(next().c_str()));
        else if (a == "--bloom") o.settings.bloomStrength = float(std::atof(next().c_str()));
        else if (a == "--bloom-threshold") o.settings.bloomThreshold = float(std::atof(next().c_str()));
        else if (a == "--bloom-mode") o.settings.bloomThresholdMode = std::atoi(next().c_str());
        else if (a == "--bloom-scale") o.settings.bloomScale = float(std::atof(next().c_str()));
        else if (a == "--bloom-tint") {
            const std::string v = next();
            float r = 1.0f, g = 1.0f, b = 1.0f;
            std::sscanf(v.c_str(), "%f,%f,%f", &r, &g, &b);
            o.settings.bloomTint = glm::vec3(r, g, b);
        }
        else if (a == "--bloom-weights") {
            const std::string v = next();
            float w0 = 0.25f, w1 = 0.25f, w2 = 0.25f, w3 = 0.25f;
            std::sscanf(v.c_str(), "%f,%f,%f,%f", &w0, &w1, &w2, &w3);
            o.settings.bloomWeights = glm::vec4(w0, w1, w2, w3);
        }
        else if (a == "--bloom-radius") o.settings.bloomBlurRadius = float(std::atof(next().c_str()));
        else if (a == "--post-preset") {
            if (next() == "kaze-unity") ukiyo::applyKazeUnityPost(o.settings);
        }
        else if (a == "--contrast-log") o.settings.contrastLogC = true;
        else if (a == "--saturation-linear") o.settings.saturationLinear = true;
        else if (a == "--saturation") o.settings.saturation = float(std::atof(next().c_str()));
        else if (a == "--contrast") o.settings.contrast = float(std::atof(next().c_str()));
        else if (a == "--vignette") o.settings.vignette = float(std::atof(next().c_str()));
        else if (a == "--grain") o.settings.grain = float(std::atof(next().c_str()));
        else if (a == "--lut") o.lut = next();
        else if (a == "--anime-knee") o.settings.animeKnee = float(std::atof(next().c_str()));
        else if (a == "--anime-toe") o.settings.animeToe = float(std::atof(next().c_str()));
        else if (a == "--no-dither") o.settings.dither = false;
        else if (a == "--lut-amount") o.settings.lutAmount = float(std::atof(next().c_str()));
        else if (a == "--white-balance") o.settings.whiteBalance = float(std::atof(next().c_str()));
        else if (a == "--shadow-tint" || a == "--highlight-tint") {
            float rgb[3] = {0.0f, 0.0f, 0.0f};
            std::sscanf(next().c_str(), "%f,%f,%f", &rgb[0], &rgb[1], &rgb[2]);
            glm::vec3& tint = a == "--shadow-tint" ? o.settings.shadowTint : o.settings.highlightTint;
            tint = glm::vec3(rgb[0], rgb[1], rgb[2]);
        }
        else if (a == "--audio") o.audio = next();
        else if (a == "--anim-lib") o.animLib = next();
        else if (a == "--anim") o.clip = next();
        else if (a == "--idle") o.idle = true;
        else if (a == "--idle-seed") o.idleSeed = uint32_t(std::atoi(next().c_str()));
        else if (a == "--hand-on-hip") o.handOnHip = next() == "left" ? 1 : 0;
        else if (a == "--no-arm-ik") o.armIK = false;
        else if (a == "--ik-debug") o.ikDebug = true;
        else if (a == "--robo-target") {
            o.roboOn = true;
            std::sscanf(next().c_str(), "%f,%f,%f", &o.roboTarget[0], &o.roboTarget[1], &o.roboTarget[2]);
        }
        else if (a == "--robo-track") { o.roboOn = true; o.roboTrack = true; }
        else if (a == "--robo-solver") o.roboFabrik = next() == "fabrik";
        else if (a == "--robo-weight") o.roboWeight = float(std::atof(next().c_str()));
        else if (a == "-h" || a == "--help") { usage(); std::exit(0); }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); std::exit(2); }
    }
    return o;
}

// Frames the model's head/upper body. Uses the live head bone position so the
// framing does not depend on the mesh's own (possibly scaled) bounds.
void frameCamera(OrbitCamera& cam, const Model& m, const Options& o) {
    cam.yaw = o.yaw;
    cam.pitch = o.pitch;
    float headY = 1.45f;
    auto it = m.humanoid.find("head");
    if (it != m.humanoid.end()) headY = m.nodes[it->second].world[3].y;
    cam.target = glm::vec3(0.0f, o.targetY > 0.0f ? o.targetY : headY * 0.82f, 0.0f);
    cam.distance = o.distance > 0.0f ? o.distance : headY * 2.1f;
}

struct AppState {
    OrbitCamera* cam = nullptr;
    bool autoOrbit = false;
    bool resetCam = false;
    bool saveShot = false;
    bool quit = false;
    bool hitRequested = false;
};

// The viewer's controls are an asset, not a switch statement: assets/input/viewer.inputactions is a Unity-shaped file,
// and driving the viewer from it means every release runs the reader, the composites, the mouse axes and the action
// phases for real. The names below are the same ones input_test checks in CI (tests/input_test.cpp §12).
struct ViewerControls {
    input::State state;
    input::Action* move = nullptr;
    input::Action* elevate = nullptr;
    input::Action* look = nullptr;
    input::Action* orbit = nullptr;
    input::Action* pan = nullptr;
    input::Action* zoom = nullptr;
    input::Action* quit = nullptr;
    input::Action* autoOrbit = nullptr;
    input::Action* resetCamera = nullptr;
    input::Action* screenshot = nullptr;
    input::Action* hit = nullptr;
    input::Action* grid = nullptr;
    input::Action* bloom = nullptr;
    input::Action* outlines = nullptr;
    input::Action* shadows = nullptr;
    input::Action* sky = nullptr;
    input::Action* aa[4] = {nullptr, nullptr, nullptr, nullptr};

    bool load(GLFWwindow* window, std::string* error) {
        input::glfw_backend::install(state, window);
        // Run from the repository root (the documented way) or from build/: try both, and report the path that was
        // actually attempted so a missing asset is obvious.
        const char* paths[] = {UKIYO_ROOT "/assets/input/viewer.inputactions", "assets/input/viewer.inputactions"};
        input::AssetLoadResult result;
        for (const char* path : paths) {
            result = input::loadAssetFile(state, path);
            if (result.ok) break;
        }
        if (!result.ok) {
            *error = result.error;
            return false;
        }
        for (const std::string& warning : result.warnings) std::fprintf(stderr, "input: %s\n", warning.c_str());
        state.enableMap("Viewer");

        auto get = [&](const char* name) { return state.action("Viewer", name); };
        move = get("Move");
        elevate = get("Elevate");
        look = get("Look");
        orbit = get("Orbit");
        pan = get("Pan");
        zoom = get("Zoom");
        quit = get("Quit");
        autoOrbit = get("AutoOrbit");
        resetCamera = get("ResetCamera");
        screenshot = get("Screenshot");
        hit = get("Hit");
        grid = get("ToggleGrid");
        bloom = get("ToggleBloom");
        outlines = get("ToggleOutlines");
        shadows = get("ToggleShadows");
        sky = get("ToggleSky");
        aa[0] = get("Aa1");
        aa[1] = get("Aa2");
        aa[2] = get("Aa3");
        aa[3] = get("Aa4");
        input::Action* required[] = {move, elevate, look, orbit, pan, zoom, quit, autoOrbit, resetCamera,
                                     screenshot, hit, grid, bloom, outlines, shadows, sky, aa[0], aa[1], aa[2], aa[3]};
        for (input::Action* a : required) {
            if (!a) {
                *error = "the viewer asset no longer has every action the viewer reads";
                return false;
            }
        }
        return true;
    }
};

// GLFW cannot poll the wheel: the callback hands it to the backend, which accumulates it into the next frame's scroll.
void onScroll(GLFWwindow*, double x, double y) { input::glfw_backend::pushScroll(x, y); }

void writePng(const std::string& path, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
    if (!stbi_write_png(path.c_str(), int(w), int(h), 4, rgba.data(), int(w * 4)))
        std::fprintf(stderr, "failed to write %s\n", path.c_str());
    else
        std::printf("wrote %s\n", path.c_str());
}

} // namespace

int main(int argc, char** argv) {
    Options opt = parseArgs(argc, argv);

    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed (is DISPLAY set? try xvfb-run)\n");
        if (opt.backend == rhi::Backend::OpenGL || !opt.headless) return 1;
    }

    GLFWwindow* window = nullptr;
    if (opt.backend == rhi::Backend::OpenGL) {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_VISIBLE, opt.headless ? GLFW_FALSE : GLFW_TRUE);
        window = glfwCreateWindow(int(opt.width), int(opt.height), "Ukiyo Engine (OpenGL)", nullptr, nullptr);
    } else if (!opt.headless) {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        window = glfwCreateWindow(int(opt.width), int(opt.height), "Ukiyo Engine (Vulkan)", nullptr, nullptr);
    }
    if (opt.backend == rhi::Backend::OpenGL && !window) {
        std::fprintf(stderr, "failed to create GL window\n");
        return 1;
    }

    try {
        rhi::DeviceConfig cfg;
        cfg.width = opt.width;
        cfg.height = opt.height;
        cfg.headless = opt.headless;
        auto device = rhi::createDevice(opt.backend, cfg, window);
        std::printf("backend: %s  device: %s\n",
                    opt.backend == rhi::Backend::OpenGL ? "OpenGL" : "Vulkan", device->deviceName().c_str());

        Renderer renderer(*device);
        if (!opt.lut.empty()) {
            std::string lutErr;
            if (!renderer.setGradingLut(opt.lut, &lutErr)) {
                std::fprintf(stderr, "%s\n", lutErr.c_str());
                return 1;
            }
        }
        ModelLoadOptions lo;
        lo.smoothNormals = opt.settings.smoothNormals;
        lo.faceSdfPath = opt.faceSdf;
        if (!opt.faceMaterial.empty()) lo.faceMaterial = opt.faceMaterial;
        auto model = loadModel(*device, opt.model, lo);
        std::printf("loaded %s (%s) materials=%zu primitives=%zu draws=%zu skins=%zu spring_joints=%zu colliders=%zu\n",
                    model->name.c_str(), model->isVRM ? "VRM 1.0" : model->isVRM0 ? "VRM 0.x" : "glTF",
                    model->materials.size(), model->primitives.size(), model->draws.size(), model->skins.size(),
                    model->springJoints.size(), model->colliders.size());

        model->physics = opt.physics;
        model->armCollision = opt.armIK;
        const glm::mat4 baseRoot = model->root;

        // Animation: UAL clips retargeted onto the model. Idle state machine, fixed clip, or a
        // forced hand-on-waist pose, all optional.
        std::unique_ptr<AnimLibrary> animLib;
        AnimPlayer animPlayer;
        const bool animOn = opt.idle || !opt.clip.empty() || opt.handOnHip >= 0;
        if (animOn) {
            animLib = AnimLibrary::load(opt.animLib);
            animPlayer.bind(*model, *animLib);
            if (!opt.clip.empty()) animPlayer.play(opt.clip, 0.0f);
            if (opt.idle) animPlayer.startIdle(opt.idleSeed);
            if (opt.handOnHip >= 0) animPlayer.forceHandOnHip(opt.handOnHip);
        }
        // Robotic arm rig: a two-bone (or FABRIK) constraint toward a world-space target.
        if (opt.roboOn) {
            Rig rig = makeRoboArmRig(*model, opt.roboFabrik);
            if (rig.constraints.empty()) {
                std::fprintf(stderr, "robo arm: model has no robo_upper_arm.L / robo_forearm.L / robo_hand.L\n");
                return 1;
            }
            rig.weight = opt.roboWeight;
            model->rigs.push_back(std::move(rig));
            std::printf("rig: %s (%s) target world (%.2f %.2f %.2f)%s\n", model->rigs.back().name.c_str(),
                        opt.roboFabrik ? "fabrik" : "two-bone", opt.roboTarget[0], opt.roboTarget[1],
                        opt.roboTarget[2], opt.roboTrack ? " tracking" : "");
        }
        // Drives the robo target for a given time. Returns the world-space target.
        auto driveRobo = [&](float t) -> glm::vec3 {
            glm::vec3 w(opt.roboTarget[0], opt.roboTarget[1], opt.roboTarget[2]);
            if (opt.roboTrack) {
                // Figure-eight in the frontal plane around the target point.
                w.x += 0.25f * std::sin(2.0f * 3.14159265f * 0.25f * t);
                w.y += 0.12f * std::sin(2.0f * 3.14159265f * 0.5f * t);
            }
            if (!model->rigs.empty() && !model->rigs[0].targets.empty())
                model->rigs[0].targets[0].position = glm::vec3(glm::inverse(model->root) * glm::vec4(w, 1.0f));
            return w;
        };
        std::string lastAnimName;
        float ikMinClear[2] = {1e9f, 1e9f}, ikMaxPen[2] = {0.0f, 0.0f};
        double roboErrSum = 0.0, roboErrMax = 0.0;
        int roboErrN = 0;
        FaceTrack face;
        if (!opt.face.empty()) {
            std::string err;
            if (!face.load(opt.face, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
            std::printf("face track %.2fs\n", face.duration());
        }
        std::vector<Model*> models{model.get()};
        OrbitCamera cam;
        cam.yaw = opt.yaw;

        // Warm-up pose so the head bone has a world transform for framing.
        device->beginFrame();
        if (animOn) animPlayer.update(*model, 0.0f, 0.0f);
        driveRobo(0.0f);
        updateModel(*model, 0.0f, glm::vec3(0.0f, 1.0f, 3.0f));
        if (opt.roboOn) {
            for (const char* nm : {"robo_root_pole", "robo_shoulder.L", "robo_upper_arm.L", "robo_forearm.L", "robo_hand.L"}) {
                const int ni = findNodeByName(*model, nm);
                if (ni >= 0) {
                    const glm::vec3 p = glm::vec3(model->nodes[ni].world[3]);
                    std::printf("robo joint %-16s world (%.3f %.3f %.3f)\n", nm, p.x, p.y, p.z);
                }
            }
        }
        device->endFrame(false);
        frameCamera(cam, *model, opt);
        const OrbitCamera baseCam = cam;

        const float fpsF = float(opt.fps > 0 ? opt.fps : 30);

        if (opt.headless) {
            const int total = opt.video.empty() ? std::max(1, opt.frames)
                                                : int(std::lround(opt.seconds * fpsF));
            FILE* ff = nullptr;
            // With --audio the silent render goes to a temp file, then the audio is muxed in.
            const std::string videoOut = opt.audio.empty() ? opt.video : opt.video + ".silent.mp4";
            if (!opt.video.empty()) {
                std::string cmd = "ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgba -s " +
                                  std::to_string(device->backbufferWidth()) + "x" +
                                  std::to_string(device->backbufferHeight()) + " -r " +
                                  std::to_string(opt.fps) +
                                  " -i - -an -c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p" +
                                  (opt.audio.empty() ? std::string(" -movflags +faststart") : std::string()) +
                                  " \"" +
                                  videoOut + "\"";
                ff = popen(cmd.c_str(), "w");
                if (!ff) { std::fprintf(stderr, "could not start ffmpeg\n"); return 1; }
            }

            auto t0 = std::chrono::steady_clock::now();
            for (int f = 0; f < total; ++f) {
                const float time = float(f) / fpsF;
                const float progress = total > 1 ? float(f) / float(total - 1) : 0.0f;
                cam = baseCam;
                cam.yaw = baseCam.yaw + progress * opt.orbitTurns * glm::two_pi<float>();

                while (!device->beginFrame()) {}
                if (opt.hitAt >= 0.0f && std::fabs(time - opt.hitAt) < 0.5f / fpsF)
                    applyHitImpulse(*model, glm::vec3(0.0f, 0.0f, -1.0f), opt.hitStrength);
                if (opt.swayAmp > 0.0f) {
                    // Root sways sideways (x) on a sine; the body leans against its acceleration.
                    const float w = 2.0f * 3.14159265f * 0.6f;
                    const float s = std::sin(w * time);
                    model->root = glm::translate(glm::mat4(1.0f), glm::vec3(opt.swayAmp * s, 0.0f, 0.0f)) * baseRoot;
                    applySway(*model, glm::vec3(-opt.swayAmp * w * w * s, 0.0f, 0.0f));
                }
                if (face.empty() == false) {
                    applyFaceChannels(*model, face.sample(time));
                }
                if (animOn) {
                    animPlayer.update(*model, time, 1.0f / fpsF);
                    if (opt.ikDebug && animPlayer.current() != lastAnimName) {
                        lastAnimName = animPlayer.current();
                        std::printf("t=%.2fs anim %s\n", time, lastAnimName.c_str());
                    }
                }
                const glm::vec3 roboGoal = driveRobo(time);
                updateModel(*model, time, cam.eye());
                if (opt.roboOn && !model->rigs.empty()) {
                    // Reach error: distance from the robo hand joint to the target.
                    const int hand = findNodeByName(*model, "robo_hand.L");
                    if (hand >= 0) {
                        const glm::vec3 h = glm::vec3(model->nodes[hand].world[3]);
                        const float err = glm::length(h - roboGoal);
                        roboErrSum += err;
                        roboErrMax = std::max(roboErrMax, double(err));
                        roboErrN++;
                    }
                }
                if (opt.ikDebug)
                    for (int s = 0; s < 2; ++s)
                        if (model->arms[s].clearance < -0.002f)
                            std::printf("ik: f=%d t=%.2f %s clearance %.4f hip %.2f\n", f, time, s ? "left" : "right",
                                        model->arms[s].clearance, model->arms[s].hipWeight);
                for (int s = 0; s < 2; ++s) {
                    ikMinClear[s] = std::min(ikMinClear[s], model->arms[s].clearance);
                    ikMaxPen[s] = std::max(ikMaxPen[s], model->arms[s].penetration);
                }
                renderer.render(cam, models, time, uint32_t(f), opt.settings);
                const bool wantCapture = ff != nullptr || (!opt.shot.empty() && f == total - 1);
                device->endFrame(wantCapture);
                if (wantCapture) {
                    auto px = device->takeCapture();
                    if (ff) std::fwrite(px.data(), 1, px.size(), ff);
                    if (!opt.shot.empty() && f == total - 1)
                        writePng(opt.shot, px, device->backbufferWidth(), device->backbufferHeight());
                }
                if (f == 0 || (f + 1) % 10 == 0 || f == total - 1) {
                    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                    std::printf("frame %d/%d  (%.2fs elapsed, %.2fs/frame)\n", f + 1, total, s, s / (f + 1));
                    std::fflush(stdout);
                }
            }
            if (opt.roboOn && roboErrN > 0)
                std::printf("robo: reach error mean %.4f m, max %.4f m over %d frames\n",
                            roboErrSum / roboErrN, roboErrMax, roboErrN);
            if (opt.ikDebug)
                std::printf("ik: min clearance right %.4f m, left %.4f m; max penetration right %.4f m, left %.4f m\n",
                            ikMinClear[0], ikMinClear[1], ikMaxPen[0], ikMaxPen[1]);
            if (ff) {
                int rc = pclose(ff);
                if (rc != 0) std::fprintf(stderr, "ffmpeg exited with %d\n", rc);
                else if (!opt.audio.empty()) {
                    std::string mux = "ffmpeg -y -loglevel error -i \"" + videoOut + "\" -i \"" + opt.audio +
                                      "\" -map 0:v -map 1:a -c:v copy -c:a aac -b:a 192k -shortest -movflags +faststart \"" +
                                      opt.video + "\"";
                    int mrc = std::system(mux.c_str());
                    std::remove(videoOut.c_str());
                    if (mrc != 0) std::fprintf(stderr, "audio mux failed (%d)\n", mrc);
                    else std::printf("wrote %s (with audio)\n", opt.video.c_str());
                } else std::printf("wrote %s\n", opt.video.c_str());
            }
            device->waitIdle();
            return 0;
        }

        // Interactive loop.
        if (!window) { std::fprintf(stderr, "interactive mode needs a window\n"); return 1; }
        AppState st;
        st.cam = &cam;
        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(window, &fbw, &fbh);
        glfwSetWindowUserPointer(window, &st);
        glfwSetScrollCallback(window, onScroll);
        ViewerControls controls;
        std::string inputError;
        if (!controls.load(window, &inputError)) throw std::runtime_error("input: " + inputError);

        auto last = std::chrono::steady_clock::now();
        uint32_t frameIndex = 0;
        int shotCount = 0;
        while (!glfwWindowShouldClose(window) && !st.quit) {
            glfwPollEvents();
            glfwGetFramebufferSize(window, &fbw, &fbh);

            auto now = std::chrono::steady_clock::now();
            const float dt = std::min(0.1f, std::chrono::duration<float>(now - last).count());
            last = now;
            const float time = float(glfwGetTime());

            // One input update a frame, after GLFW has collected the events. Every read below is an action.
            controls.state.update(dt);
            RenderSettings& s = opt.settings;
            if (controls.quit->triggered()) st.quit = true;
            if (controls.autoOrbit->triggered()) st.autoOrbit = !st.autoOrbit;
            if (controls.resetCamera->triggered()) st.resetCam = true;
            if (controls.screenshot->triggered()) st.saveShot = true;
            if (controls.hit->triggered()) st.hitRequested = true;
            if (controls.grid->triggered()) s.grid = !s.grid;
            if (controls.bloom->triggered()) s.bloom = !s.bloom;
            if (controls.outlines->triggered()) s.outlines = !s.outlines;
            if (controls.shadows->triggered()) s.shadows = !s.shadows;
            if (controls.sky->triggered()) s.sky = !s.sky;
            for (int i = 0; i < 4; ++i)
                if (controls.aa[i]->triggered()) s.aaMode = i;

            // Look is <Mouse>/delta: pixels, y up, the way Unity reports it.
            const glm::vec2 look = controls.look->readVector2();
            if (controls.orbit->pressed()) cam.orbit(-look.x * 0.006f, -look.y * 0.004f);
            if (controls.pan->pressed() && fbh > 0) cam.pan(look.x, -look.y, float(fbh));
            const float wheel = controls.zoom->readVector2().y;
            if (wheel != 0.0f) cam.zoom(wheel);

            if (st.resetCam) { cam = baseCam; st.resetCam = false; }
            if (st.autoOrbit) cam.yaw += dt * 0.35f;
            const float speed = 1.2f * dt;
            const glm::vec2 move = controls.move->readVector2();   // x right, y forward
            if (move.x != 0.0f || move.y != 0.0f) cam.moveTargetPlanar(move.y * speed, move.x * speed);
            const float elevate = controls.elevate->readFloat();   // E positive, Q negative
            if (elevate != 0.0f) cam.moveTargetVertical(elevate * speed);

            if (!device->beginFrame()) {
                glfwWaitEventsTimeout(0.02);
                continue;
            }
            if (st.hitRequested) {
                applyHitImpulse(*model, glm::vec3(0.0f, 0.0f, -1.0f), opt.hitStrength);
                st.hitRequested = false;
            }
            driveRobo(float(time));
            updateModel(*model, time, cam.eye());
            renderer.render(cam, models, time, frameIndex++, opt.settings);
            device->endFrame(st.saveShot);
            if (st.saveShot) {
                auto px = device->takeCapture();
                writePng("renders/screenshot_" + std::to_string(++shotCount) + ".png", px,
                         device->backbufferWidth(), device->backbufferHeight());
                st.saveShot = false;
            }
        }
        device->waitIdle();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
