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
    } else if (masked == AMOTION_EVENT_ACTION_MOVE) {
        const size_t count = AMotionEvent_getPointerCount(event);
        for (size_t i = 0; i < count; ++i) {
            const int32_t pid = AMotionEvent_getPointerId(event, i);
            const float px = AMotionEvent_getX(event, i);
            const float py = AMotionEvent_getY(event, i);
            if (gStick.active && pid == gStick.id) {
                gStick.x = px;
                gStick.y = py;
                setMoveKeys(app, px - gStick.originX, py - gStick.originY);
            } else if (gLook.active && pid == gLook.id) {
                gLook.x = px;
                gLook.y = py;
            }
        }
    } else if (masked == AMOTION_EVENT_ACTION_UP || masked == AMOTION_EVENT_ACTION_POINTER_UP ||
               masked == AMOTION_EVENT_ACTION_CANCEL) {
        Touch& t = (gStick.active && id == gStick.id) ? gStick : gLook;
        if (t.active && t.id == id) {
            t.active = false;
            t.id = -1;
            if (t.stick) setMoveKeys(app, 0.0f, 0.0f);
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

    ukiyo::PlayerApp app(false);   // the game has no audio of its own yet, and a phone has no device to spare
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

    std::unique_ptr<rhi::Device> device;
    try {
        rhi::DeviceConfig cfg;
        cfg.width = static_cast<uint32_t>(ANativeWindow_getWidth(state->window));
        cfg.height = static_cast<uint32_t>(ANativeWindow_getHeight(state->window));
        rhi::NativeWindow native;
        native.android = state->window;
        device = rhi::createDevice(rhi::Backend::Vulkan, cfg, native);
    } catch (const std::exception& e) {
        LOGE("no Vulkan device: %s", e.what());
        return;
    }
    LOGI("device: %s", device->deviceName().c_str());

    ukiyo::Renderer renderer(*device);
    ukiyo::OrbitCamera cam;
    cam.target = app.scene().cameraTarget;
    cam.distance = app.scene().cameraDistance;
    cam.yaw = app.scene().cameraYaw;
    cam.pitch = app.scene().cameraPitch;
    cam.fovY = app.scene().cameraFovY;
    renderer.setSceneLights(app.scene().lights);

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
    if (font.loaded()) {
        hud.setFont(&font);
        renderer.setHudAtlas(&font);
    }

    // One model per instance that has one, loaded before the first frame: the court is small and a phone installs once.
    std::vector<std::unique_ptr<ukiyo::Model>> models;
    std::vector<size_t> modelForInstance(app.instances().size(), SIZE_MAX);
    for (size_t i = 0; i < app.instances().size(); ++i) {
        const ukiyo::PlayerInstance& in = app.instances()[i];
        if (in.modelPath.empty()) continue;
        ukiyo::ModelLoadOptions mo;
        mo.preserveAlphaTestCoverage = true;
        try {
            models.push_back(ukiyo::loadModel(*device, in.modelPath, mo));
            modelForInstance[i] = models.size() - 1;
        } catch (const std::exception& e) {
            LOGE("could not load %s: %s", in.modelPath.c_str(), e.what());
        }
    }

    const ukiyo::RenderSettings settings = app.scene().settings;
    const ukiyo::PlayerInstance* hero = characterOf(app);
    auto last = std::chrono::steady_clock::now();
    double playTime = app.playTime();
    uint32_t frameIndex = 0;

    while (!state->destroyRequested) {
        int events = 0;
        android_poll_source* source = nullptr;
        while (ALooper_pollOnce(0, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
            if (source) source->process(state, source);
            if (state->destroyRequested) break;
        }
        if (state->destroyRequested) break;
        if (!state->window) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
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

        if (!device->beginFrame()) continue;
        std::vector<ukiyo::Model*> drawList;
        for (size_t i = 0; i < app.instances().size(); ++i) {
            if (i >= modelForInstance.size() || modelForInstance[i] == SIZE_MAX) continue;
            ukiyo::Model& m = *models[modelForInstance[i]];
            m.placement = app.instances()[i].world;
            ukiyo::applyPlacement(m);
            ukiyo::updateModel(m, float(playTime), cam.eye());
            drawList.push_back(&m);
        }
        renderer.render(cam, drawList, float(playTime), frameIndex++, settings);
        if (font.loaded()) {
            hud.setScreen(float(device->backbufferWidth()), float(device->backbufferHeight()));
            // Pixels, not points: the layout is authored for 720p, so a 1080p phone gets the same proportions.
            const float scale = std::max(1.0f, float(device->backbufferHeight()) / 720.0f);
            ukiyo::drawPlayerHud(hud, app.hudText(renderer.lastDrawCalls(), renderer.lastTriangles()), scale);
            renderer.drawHud(hud.vertices(), device->backbufferWidth(), device->backbufferHeight());
        }
        device->endFrame(false);
    }

    device->waitIdle();
    app.stop();
}
