// Lantern Run on Android.
//
// The desktop player is a thin layer over PlayerApp (logic) and Renderer (drawing); this is the same layer with the
// platform swapped: the activity's surface instead of a GLFW window, touch instead of a keyboard and mouse, and the
// APK's assets instead of the checkout. Nothing about the game changes — the project, the scene, the script and the
// components are the files the desktop player runs.
//
// The APK ships the packaged project under `assets/` and an `assets/files.txt` listing every file. On first launch the
// app copies that tree into its internal storage and chdirs there, so every path in the project (../../assets/...,
// editor/res/fonts/..., shaders/spv/...) resolves exactly as it does on a desktop. Staging costs a few megabytes of
// flash and buys the property that matters: one project layout, one set of relative paths, two platforms.
#include <android/asset_manager.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <glm/glm.hpp>

#include "engine/camera.h"
#include "engine/hud.h"
#include "engine/model.h"
#include "engine/renderer.h"
#include "engine/text_atlas.h"
#include "player/hud_layout.h"
#include "player/player_app.h"
#include "rhi/rhi.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ukiyo", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ukiyo", __VA_ARGS__)

namespace {

// ---- Asset staging ------------------------------------------------------------------------------
//
// An APK is a zip: its assets cannot be opened by path, so the player copies them out once. The list comes from the
// package itself rather than from a directory walk, because the NDK's asset API cannot tell a directory from a file
// without trying to open it, and a failed open then looks exactly like an empty directory.

bool readAsset(AAssetManager* mgr, const std::string& name, std::vector<uint8_t>* out) {
    AAsset* a = AAssetManager_open(mgr, name.c_str(), AASSET_MODE_STREAMING);
    if (!a) return false;
    const off64_t length = AAsset_getLength64(a);
    out->resize(static_cast<size_t>(length));
    const int read = AAsset_read(a, out->data(), out->size());
    AAsset_close(a);
    return read == static_cast<int>(out->size());
}

void makeParents(const std::string& path) {
    // Every directory above `path`, not `path` itself.
    for (size_t i = 1; i < path.size(); ++i) {
        if (path[i] != '/') continue;
        ::mkdir(path.substr(0, i).c_str(), 0755);   // EEXIST is the normal case after the first file in a folder
    }
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    makeParents(path);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    return written == bytes.size();
}

// Copies every file `files.txt` names. Returns the number written, or -1 when the list or one of its files is missing.
int stageAssets(android_app* state, const std::string& root) {
    AAssetManager* mgr = state->activity->assetManager;
    std::vector<uint8_t> list;
    if (!readAsset(mgr, "files.txt", &list)) {
        LOGE("files.txt is missing from the APK");
        return -1;
    }
    const std::string names(reinterpret_cast<const char*>(list.data()), list.size());
    size_t start = 0;
    int copied = 0;
    while (start < names.size()) {
        const size_t end = names.find('\n', start);
        std::string name = names.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? names.size() : end + 1;
        while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
        if (name.empty()) continue;
        std::vector<uint8_t> bytes;
        if (!readAsset(mgr, name, &bytes)) {
            LOGE("asset %s could not be read", name.c_str());
            return -1;
        }
        if (!writeFile(root + "/" + name, bytes)) {
            LOGE("could not write %s", (root + "/" + name).c_str());
            return -1;
        }
        ++copied;
    }
    return copied;
}

// ---- Touch --------------------------------------------------------------------------------------
//
// The court is walked with the left thumb and looked at with the right one. A touch that starts on the left half is a
// virtual stick: its first position is the centre, and the direction from there feeds the character controller's WASD.
// A touch that starts on the right half orbits the camera. Neither is drawn: the game has one verb, and a stick sprite
// over a lantern court would be the loudest thing on the screen.

struct Touch {
    bool active = false;
    bool stick = false;
    int32_t id = -1;
    float originX = 0.0f, originY = 0.0f;
    float x = 0.0f, y = 0.0f;
};

Touch gStick;
Touch gLook;

// Every finger also feeds the input system itself, not only the keyboard emulation above: a project whose action is
// bound to "<Touchscreen>/primaryTouch/press" (tap to light, tap to pick up) reads the same event a script's WASD
// does. Positions are screen fractions so a binding survives any resolution.
void feedTouch(android_app* state, ukiyo::PlayerApp* app, const Touch& t, bool down, float px, float py, float dx,
               float dy) {
    const float w = state->window ? float(ANativeWindow_getWidth(state->window)) : 0.0f;
    const float h = state->window ? float(ANativeWindow_getHeight(state->window)) : 0.0f;
    const glm::vec2 pos(w > 0.0f ? px / w : 0.0f, h > 0.0f ? py / h : 0.0f);
    const glm::vec2 delta(w > 0.0f ? dx / w : 0.0f, h > 0.0f ? dy / h : 0.0f);
    app->setTouch(t.stick ? 0 : 1, down, pos, delta);
}

void setMoveKeys(ukiyo::PlayerApp* app, float dx, float dy) {
    // A dead zone, so a thumb resting on the glass does not walk the hero off the court, and a 38 % cone per direction
    // so the four keys do not all fire at once.
    const float len = std::sqrt(dx * dx + dy * dy);
    const bool moving = len > 18.0f;
    app->setKey("W", moving && dy < -len * 0.38f);
    app->setKey("S", moving && dy > len * 0.38f);
    app->setKey("A", moving && dx < -len * 0.38f);
    app->setKey("D", moving && dx > len * 0.38f);
}

int32_t onInput(android_app* state, AInputEvent* event) {
    auto* app = static_cast<ukiyo::PlayerApp*>(state->userData);
    if (!app) return 0;
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) return 0;

    const int32_t action = AMotionEvent_getAction(event);
    const int32_t masked = action & AMOTION_EVENT_ACTION_MASK;
    const int32_t index =
        (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    const int32_t id = AMotionEvent_getPointerId(event, static_cast<size_t>(index));
    const float x = AMotionEvent_getX(event, static_cast<size_t>(index));
    const float y = AMotionEvent_getY(event, static_cast<size_t>(index));
    const float halfWidth = state->window ? ANativeWindow_getWidth(state->window) * 0.5f : 0.0f;

    if (masked == AMOTION_EVENT_ACTION_DOWN || masked == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        Touch& t = (x < halfWidth) ? gStick : gLook;
        if (t.active) return 1;   // the finger that owns this side keeps it until it lifts
        t.active = true;
        t.stick = (x < halfWidth);
        t.id = id;
        t.originX = x;
        t.originY = y;
        t.x = x;
        t.y = y;
        feedTouch(state, app, t, true, x, y, 0.0f, 0.0f);
    } else if (masked == AMOTION_EVENT_ACTION_MOVE) {
        const size_t count = AMotionEvent_getPointerCount(event);
        for (size_t i = 0; i < count; ++i) {
            const int32_t pid = AMotionEvent_getPointerId(event, i);
            const float px = AMotionEvent_getX(event, i);
            const float py = AMotionEvent_getY(event, i);
            if (gStick.active && pid == gStick.id) {
                const float dx = px - gStick.x;
                const float dy = py - gStick.y;
                gStick.x = px;
                gStick.y = py;
                setMoveKeys(app, px - gStick.originX, py - gStick.originY);
                feedTouch(state, app, gStick, true, px, py, dx, dy);
            } else if (gLook.active && pid == gLook.id) {
                const float dx = px - gLook.x;
                const float dy = py - gLook.y;
                gLook.x = px;
                gLook.y = py;
                feedTouch(state, app, gLook, true, px, py, dx, dy);
            }
        }
    } else if (masked == AMOTION_EVENT_ACTION_UP || masked == AMOTION_EVENT_ACTION_POINTER_UP ||
               masked == AMOTION_EVENT_ACTION_CANCEL) {
        Touch& t = (gStick.active && id == gStick.id) ? gStick : gLook;
        if (t.active && t.id == id) {
            const float lx = t.x;
            const float ly = t.y;
            t.active = false;
            t.id = -1;
            if (t.stick) setMoveKeys(app, 0.0f, 0.0f);
            feedTouch(state, app, t, false, lx, ly, 0.0f, 0.0f);
        }
    }
    return 1;
}

// The instance the camera follows: the one the player drives.
const ukiyo::PlayerInstance* characterOf(const ukiyo::PlayerApp& app) {
    for (const ukiyo::PlayerInstance& in : app.instances())
        if (in.components.character.enabled) return &in;
    return nullptr;
}

} // namespace

void android_main(android_app* state) {
    app_dummy();
    state->onInputEvent = onInput;

    // Stage the package and work from it. Every path below is relative to this directory, so the project is the same
    // tree the desktop player and the editor open.
    const std::string root = state->activity->internalDataPath;
    const int staged = stageAssets(state, root);
    if (staged < 0) {
        LOGE("asset staging failed");
        return;
    }
    LOGI("staged %d asset(s) into %s", staged, root.c_str());
    if (::chdir(root.c_str()) != 0) {
        LOGE("could not enter %s", root.c_str());
        return;
    }

    // Audio is on: miniaudio opens AAudio on API 26+ (this game's floor is 33) and falls back to OpenSL ES. If the
    // device refuses, AudioEngine reports it and the game runs silent - one path, three platforms, no #ifdef.
    ukiyo::PlayerApp app(true);
    std::string error;
    const std::string project = "games/lantern-run";
    if (!app.start(project + "/Build/build.json", project, &error)) {
        LOGE("the game did not start: %s", error.c_str());
        return;
    }
    state->userData = &app;
    LOGI("%s: %zu instances, %d script(s) running", app.title().c_str(), app.instances().size(), app.scriptsRunning());
    for (const std::string& w : app.warnings()) LOGE("warning: %s", w.c_str());

    rhi::setShaderRoot("shaders/spv");

    // Camera and text are CPU-side and outlive every surface: the view the user left is the view they come back to,
    // and the atlas is not rebuilt just because the swapchain was.
    ukiyo::OrbitCamera cam;
    cam.target = app.scene().cameraTarget;
    cam.distance = app.scene().cameraDistance;
    cam.yaw = app.scene().cameraYaw;
    cam.pitch = app.scene().cameraPitch;
    cam.fovY = app.scene().cameraFovY;

    // Text: the manifest names the font, and the package carries it at its repo-relative path.
    ukiyo::FontAtlas font;
    if (!app.manifest().font.empty()) {
        const std::string fontPath = project + "/" + app.manifest().font;
        const bool ok = app.manifest().msdf ? font.loadMsdfFromFile(fontPath, 40.0f, 6.0f, 2048)
                                            : font.loadFromFile(fontPath, 20.0f, 2048);
        if (ok) font.preloadAscii();
        else LOGE("could not load the font %s", fontPath.c_str());
    }
    ukiyo::HudCanvas hud;
    if (font.loaded()) hud.setFont(&font);

    // ---- The surface is not permanent -----------------------------------------------------------------
    //
    // Android hands the activity a window, takes it away when the app is backgrounded, and hands back a *different*
    // one on resume. A swapchain built from the old window cannot be reused, so the device, the renderer and every
    // model that lives on the GPU have to be built from the window that exists now and dropped when it goes. This
    // used to be done once, before the loop, with the window the activity happened to start with; the app then
    // rendered into a destroyed surface after the first resume, which on most phones is the first time the user
    // switches away and back.
    //
    // Everything that does not depend on the GPU - the game, the camera, the font, the HUD vertices - stays alive
    // across the whole run, so a resume costs a device creation and a model load, not a restart of the game.
    struct Session {
        std::unique_ptr<rhi::Device> device;
        std::unique_ptr<ukiyo::Renderer> renderer;
        std::vector<std::unique_ptr<ukiyo::Model>> models;
        std::vector<size_t> modelForInstance;
    };
    std::unique_ptr<Session> session;
    double playTime = app.playTime();
    uint32_t frameIndex = 0;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();

    // Builds everything that needs the GPU over the window the activity holds right now. False when there is no
    // window, or when no Vulkan device could be created: the loop then waits, because a phone that has just resumed
    // is a phone whose surface may not exist for another frame or two.
    auto openSession = [&]() -> bool {
        if (!state->window || session) return session != nullptr;
        auto built = std::make_unique<Session>();
        try {
            rhi::DeviceConfig cfg;
            cfg.width = static_cast<uint32_t>(ANativeWindow_getWidth(state->window));
            cfg.height = static_cast<uint32_t>(ANativeWindow_getHeight(state->window));
            rhi::NativeWindow native;
            native.android = state->window;
            built->device = rhi::createDevice(rhi::Backend::Vulkan, cfg, native);
        } catch (const std::exception& e) {
            LOGE("no Vulkan device: %s", e.what());
            return false;
        }
        LOGI("device: %s (%ux%u)", built->device->deviceName().c_str(),
             ANativeWindow_getWidth(state->window), ANativeWindow_getHeight(state->window));

        built->renderer = std::make_unique<ukiyo::Renderer>(*built->device);
        built->renderer->setSceneLights(app.scene().lights);
        if (font.loaded()) built->renderer->setHudAtlas(&font);

        // One model per instance that has one. A resumed app reloads them, which is the price of the phone having
        // taken the old device away; the scene, the script state and the physics world are untouched.
        built->modelForInstance.assign(app.instances().size(), SIZE_MAX);
        for (size_t i = 0; i < app.instances().size(); ++i) {
            const ukiyo::PlayerInstance& in = app.instances()[i];
            if (in.modelPath.empty()) continue;
            ukiyo::ModelLoadOptions mo;
            mo.preserveAlphaTestCoverage = true;
            try {
                built->models.push_back(ukiyo::loadModel(*built->device, in.modelPath, mo));
                built->modelForInstance[i] = built->models.size() - 1;
            } catch (const std::exception& e) {
                LOGE("could not load %s: %s", in.modelPath.c_str(), e.what());
            }
        }
        session = std::move(built);
        // The clock is meaningless across a pause: the gap between the last frame before backgrounding and the first
        // one after resume is not a frame the game should simulate.
        last = std::chrono::steady_clock::now();
        return true;
    };

    auto closeSession = [&]() {
        if (!session) return;
        // Wait for the device to be idle before anything it owns is destroyed: a frame may still be in flight, and
        // destroying a swapchain under it is a use-after-free inside the driver.
        if (session->device) session->device->waitIdle();
        session.reset();
        LOGI("surface lost: GPU resources released");
    };

    const ukiyo::RenderSettings settings = app.scene().settings;
    const ukiyo::PlayerInstance* hero = characterOf(app);

    while (!state->destroyRequested) {
        int events = 0;
        android_poll_source* source = nullptr;
        while (ALooper_pollOnce(session ? 0 : 16, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
            if (source) source->process(state, source);
            if (state->destroyRequested) break;
        }
        if (state->destroyRequested) break;

        // The native-activity glue clears state->window on APP_CMD_TERM_WINDOW and sets it again on APP_CMD_INIT_WINDOW,
        // so the surface this app draws into is exactly "the window the activity currently holds".
        if (!state->window) {
            closeSession();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }
        if (!openSession()) {
            // The window exists but no device could be built over it. Waiting is the only thing that helps: the
            // driver may still be releasing the old one.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        dt = std::min(dt, 0.1f);
        if (dt <= 0.0f) dt = 1.0f / 60.0f;
        playTime += dt;
        app.reportFrame(dt);

        // The right thumb orbits; the target follows the hero, so walking never takes the court off screen.
        if (gLook.active) {
            cam.orbit(-(gLook.x - gLook.originX) * 0.004f, -(gLook.y - gLook.originY) * 0.004f);
            gLook.originX = gLook.x;
            gLook.originY = gLook.y;
        }
        app.tick(dt);
        if (hero) {
            const glm::vec3 want = hero->position + glm::vec3(0.0f, 1.1f, 0.0f);
            const float k = 1.0f - std::exp(-8.0f * dt);
            cam.target = glm::mix(cam.target, want, k);
        }

        rhi::Device& device = *session->device;
        if (!device.beginFrame()) continue;
        std::vector<ukiyo::Model*> drawList;
        for (size_t i = 0; i < app.instances().size(); ++i) {
            if (i >= session->modelForInstance.size() || session->modelForInstance[i] == SIZE_MAX) continue;
            ukiyo::Model& m = *session->models[session->modelForInstance[i]];
            m.placement = app.instances()[i].world;
            ukiyo::applyPlacement(m);
            ukiyo::updateModel(m, float(playTime), cam.eye());
            drawList.push_back(&m);
        }
        session->renderer->render(cam, drawList, float(playTime), frameIndex++, settings);
        if (font.loaded()) {
            hud.setScreen(float(device.backbufferWidth()), float(device.backbufferHeight()));
            // Pixels, not points: the layout is authored for 720p, so a 1080p phone gets the same proportions.
            const float scale = std::max(1.0f, float(device.backbufferHeight()) / 720.0f);
            ukiyo::drawPlayerHud(hud, app.hudText(session->renderer->lastDrawCalls(), session->renderer->lastTriangles()),
                                 scale);
            session->renderer->drawHud(hud.vertices(), device.backbufferWidth(), device.backbufferHeight());
        }
        device.endFrame(false);
    }

    closeSession();
    app.stop();
}
