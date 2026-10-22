// Ukiyo player: runs a built game.
//
//   ukiyo_player --build Build/build.json --project .            windowed (Vulkan by default)
//   ukiyo_player --backend gl                                    force OpenGL
//   ukiyo_player --headless --shot out/player.png --seconds 3    render a fixed number of seconds and capture
//   ukiyo_player --load-budget 2                                 load the scene with a 2 ms budget per frame
//   ukiyo_player --wireframe                                     draw the geometry as its edges
//   ukiyo_player --selftest                                      headless logic check, no GPU
//
// The editor used to be the only way to see a scene, with its docks, gizmos and debug overlays. This is the other half of
// the Build page: it reads the manifest that page writes, loads the start scene, runs the same World the editor's play
// mode runs, and draws the game with a HUD and nothing else.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/async_load.h"
#include "engine/camera.h"
#include "engine/hud.h"
#include "engine/model.h"
#include "engine/renderer.h"
#include "engine/text_atlas.h"
#include "player/hud_layout.h"
#include "player/player_app.h"
#include "rhi/rhi.h"
#include "stb_image_write.h"

namespace {

struct Options {
    std::string build = "Build/build.json";
    std::string project = ".";
    rhi::Backend backend = rhi::Backend::Vulkan;
    bool headless = false;
    bool selftest = false;
    bool hud = true;
    bool wireframe = false;   // --wireframe: the debug view, for a screenshot or a look at the geometry
    double loadBudgetMs = 0.0;   // --load-budget: milliseconds of scene loading a frame may pay for (0 = all at once)
    std::string shot;
    float seconds = 0.0f;
    std::string shaders;   // --shaders: the compiled shaders this build shipped (a package's shaders/ folder)
};

Options parseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string* out) {
            if (i + 1 < argc) *out = argv[++i];
        };
        if (a == "--build") next(&o.build);
        else if (a == "--project") next(&o.project);
        else if (a == "--backend") {
            std::string b;
            next(&b);
            o.backend = b == "gl" ? rhi::Backend::OpenGL : rhi::Backend::Vulkan;
        } else if (a == "--headless") o.headless = true;
        else if (a == "--selftest") o.selftest = true;
        else if (a == "--no-hud") o.hud = false;
        else if (a == "--wireframe") o.wireframe = true;
        else if (a == "--load-budget") {
            std::string ms;
            next(&ms);
            o.loadBudgetMs = std::max(0.0, std::atof(ms.c_str()));
        }
        else if (a == "--shaders") {
            next(&o.shaders);
        } else if (a == "--shot") {
            next(&o.shot);
            o.headless = true;
        } else if (a == "--seconds") {
            std::string s;
            next(&s);
            o.seconds = std::stof(s);
        } else if (a == "--help" || a == "-h") {
            std::printf("usage: ukiyo_player [--build Build/build.json] [--project dir] [--backend vk|gl]\n"
                        "                    [--headless --shot out.png --seconds N] [--no-hud] [--selftest]\n"
                        "                    [--load-budget MS] [--wireframe] [--shaders DIR]\n");
            std::exit(0);
        }
    }
    return o;
}

void keyCallback(GLFWwindow* w, int key, int, int action, int) {
    auto* app = static_cast<ukiyo::PlayerApp*>(glfwGetWindowUserPointer(w));
    if (!app) return;
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        app->requestQuit();
        return;
    }
    const std::string name = ukiyo::glfwKeyNameToQt(key, glfwGetKeyName(key, 0));
    if (!name.empty() && (action == GLFW_PRESS || action == GLFW_RELEASE)) app->setKey(name, action == GLFW_PRESS);
}

int runSelfTest(int argc, char** argv) {
    // The same path the window takes, minus the device: manifest, scene, World, sixty ticks, the HUD text. It runs on a
    // machine with no GPU, which is what CI has.
    std::string build = "Build/build.json";
    std::string project = ".";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--build" && i + 1 < argc) build = argv[++i];
        else if (a == "--project" && i + 1 < argc) project = argv[++i];
    }
    ukiyo::PlayerApp app(false);
    std::string error;
    if (!app.start(build, project, &error)) {
        std::fprintf(stderr, "player selftest: %s\n", error.c_str());
        return 1;
    }
    for (int i = 0; i < 60; ++i) {
        app.reportFrame(1.0f / 60.0f);
        app.tick(1.0f / 60.0f);
    }
    const auto text = app.hudText(0, 0);
    std::printf("player selftest: %s, %zu instances, %.0f fps, title \"%s\", stats \"%s\"\n",
                app.title().c_str(), app.instances().size(), double(app.fps()), text.title.c_str(), text.stats.c_str());
    app.stop();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Options opt = parseArgs(argc, argv);
    if (opt.selftest) return runSelfTest(argc, argv);

    ukiyo::PlayerApp app(!opt.headless);
    std::string error;
    if (!app.start(opt.build, opt.project, &error)) {
        std::fprintf(stderr, "player: %s\n", error.c_str());
        return 2;
    }

    if (!glfwInit()) {
        std::fprintf(stderr, "player: glfwInit failed (try xvfb-run, or --backend vk --headless)\n");
        return 1;
    }
    GLFWwindow* window = nullptr;
    if (opt.backend == rhi::Backend::OpenGL) {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_VISIBLE, opt.headless ? GLFW_FALSE : GLFW_TRUE);
        window = glfwCreateWindow(app.manifest().window.width, app.manifest().window.height, app.title().c_str(),
                                  nullptr, nullptr);
        if (!window) {
            std::fprintf(stderr, "player: could not create the GL window\n");
            return 1;
        }
    } else if (!opt.headless) {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        window = glfwCreateWindow(app.manifest().window.width, app.manifest().window.height, app.title().c_str(),
                                  nullptr, nullptr);
        if (!window) {
            std::fprintf(stderr, "player: could not create the window\n");
            return 1;
        }
    }
    if (window) {
        glfwSetWindowUserPointer(window, &app);
        glfwSetKeyCallback(window, keyCallback);
    }

    try {
        rhi::DeviceConfig cfg;
        cfg.width = static_cast<uint32_t>(app.manifest().window.width);
        cfg.height = static_cast<uint32_t>(app.manifest().window.height);
        cfg.headless = opt.headless;
        // A packaged game carries its own shaders; a build-tree run keeps the directory CMake compiled them into.
        if (!opt.shaders.empty()) rhi::setShaderRoot(opt.shaders);
        auto device = rhi::createDevice(opt.backend, cfg, window);
        ukiyo::Renderer renderer(*device);

        // Text. The manifest names the font; without one the HUD still draws its panels, just no words.
        //
        // The page is a multi-channel distance field generated at 40 px (docs/MSDF.md): one page then serves the whole
        // HUD at any size, so a heading and a caption are equally sharp, and the debug overlay can be drawn larger
        // without regenerating anything. A manifest that sets "msdf": false gets the old coverage page at 20 px.
        ukiyo::FontAtlas font;
        if (!app.manifest().font.empty()) {
            const std::string path = opt.project + "/" + app.manifest().font;
            const bool ok = app.manifest().msdf ? font.loadMsdfFromFile(path, 40.0f, 6.0f, 2048)
                                                : font.loadFromFile(path, 20.0f, 2048);
            if (!ok) {
                std::fprintf(stderr, "player: could not load the font %s\n", path.c_str());
            } else {
                font.preloadAscii();
            }
        }
        ukiyo::HudCanvas hud;
        if (font.loaded()) {
            hud.setFont(&font);
            renderer.setHudAtlas(&font);
        }

        ukiyo::OrbitCamera cam;
        cam.target = app.scene().cameraTarget;
        cam.distance = app.scene().cameraDistance;
        cam.yaw = app.scene().cameraYaw;
        cam.pitch = app.scene().cameraPitch;
        cam.fovY = app.scene().cameraFovY;

        // The scene's lights (item 6): a scene that authors lights is shaded with them, and a key light in the scene
        // becomes the sun. A scene that authors none keeps the engine's two stylistic fills.
        renderer.setSceneLights(app.scene().lights);
        std::printf("player: %zu scene light%s\n", app.scene().lights.size(),
                    app.scene().lights.size() == 1 ? "" : "s");

        // Models follow the scene generation: a script's loadScene replaces every model.
        std::vector<std::unique_ptr<ukiyo::Model>> models;
        std::vector<size_t> modelForInstance;
        uint32_t loadedGeneration = UINT32_MAX;
        // Loading, with or without a frame budget (Flax 1.11's async scene loading). A model is one step: loaded whole
        // or not at all, so a frame that runs out of budget never leaves a half-built model behind, and the next frame
        // carries on from the instance it stopped at. A budget of 0 is the synchronous path — the scene is ready before
        // the first frame, which is what the self-test and a still frame want.
        ukiyo::BudgetedLoader sceneLoader;
        uint32_t loadGeneration = 0;   // the generation the queued steps belong to
        const auto finishLoad = [&] {
            // LOD switches are authored per instance; apply them once, after loading.
            for (size_t i = 0; i < app.instances().size(); ++i) {
                if (modelForInstance[i] == SIZE_MAX) continue;
                const auto& lod = app.scene().instances[i].lod;
                if (!lod.empty()) ukiyo::buildModelLods(*device, *models[modelForInstance[i]], lod);
            }
            loadedGeneration = loadGeneration;
        };
        auto beginModelLoad = [&]() {
            loadGeneration = app.sceneGeneration();
            sceneLoader.cancel();
            models.clear();   // steps below capture this vector; clearing it first is what makes a reload a reload
            modelForInstance.assign(app.instances().size(), SIZE_MAX);
            for (size_t i = 0; i < app.instances().size(); ++i) {
                const ukiyo::PlayerInstance in = app.instances()[i];
                if (in.modelPath.empty()) continue;
                sceneLoader.push(in.modelPath, [&, i, in] {
                    ukiyo::ModelLoadOptions mo;
                    mo.preserveAlphaTestCoverage = true;
                    try {
                        models.push_back(ukiyo::loadModel(*device, in.modelPath, mo));
                    } catch (const std::exception& e) {
                        std::fprintf(stderr, "player: %s: %s\n", in.modelPath.c_str(), e.what());
                        return;
                    }
                    modelForInstance[i] = models.size() - 1;
                });
            }
            sceneLoader.push("finish", finishLoad);
            if (opt.loadBudgetMs <= 0.0) sceneLoader.runAll();
        };
        auto reloadModels = [&]() { beginModelLoad(); };   // budgeted loads finish in the frame loop

        // What the game actually started with. A script that failed to compile leaves the game running but silent, so
        // the count and the warnings are printed once: CI greps them, and a shipped game's log says why it feels empty.
        std::printf("player: %d script%s running\n", app.scriptsRunning(), app.scriptsRunning() == 1 ? "" : "s");
        for (const std::string& w : app.warnings()) std::fprintf(stderr, "player: warning: %s\n", w.c_str());

        // Mouse orbit is polled from the window each frame; a game that needs deltas can add callbacks on top.
        bool dragging = false;
        double lastX = 0.0, lastY = 0.0;
        bool hudOn = opt.hud;
        bool f1WasDown = false;
        bool f5WasDown = false;
        bool f9WasDown = false;
        const std::string quickSave = opt.project + "/Build/quicksave.uksv";
        auto startTime = std::chrono::steady_clock::now();
        auto last = startTime;
        float capturedSeconds = 0.0f;
        std::vector<uint8_t> shot;
        // The scene's settings for this run, with the one the command line is allowed to override: a debug view belongs
        // to the person looking at the frame, not to the project.
        ukiyo::RenderSettings frameSettings = app.scene().settings;
        frameSettings.wireframe = opt.wireframe;

        while (app.running()) {
            const auto now = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(now - last).count();
            last = now;
            dt = std::min(dt, 0.1f);
            if (dt <= 0.0f) dt = 1.0f / 60.0f;
            app.reportFrame(dt);
            capturedSeconds += dt;

            glfwPollEvents();
            if (window && !opt.headless) {
                double mx = 0.0, my = 0.0;
                glfwGetCursorPos(window, &mx, &my);
                const bool down = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                if (down && dragging) {
                    cam.orbit(float(mx - lastX) * 0.006f, float(my - lastY) * 0.006f);
                }
                dragging = down;
                lastX = mx;
                lastY = my;
                if (window && glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) cam.moveTargetPlanar(3.0f * dt, 0.0f);
                if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) cam.moveTargetPlanar(-3.0f * dt, 0.0f);
                if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) cam.moveTargetPlanar(0.0f, 3.0f * dt);
                if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) cam.moveTargetPlanar(0.0f, -3.0f * dt);
                const bool f1Down = glfwGetKey(window, GLFW_KEY_F1) == GLFW_PRESS;
                if (f1Down && !f1WasDown) hudOn = !hudOn;
                f1WasDown = f1Down;
                // Quick save and load, the two keys every game has. The HUD's warning line reports the result on the
                // next frame, so a failed load is visible rather than silent.
                const bool f5Down = glfwGetKey(window, GLFW_KEY_F5) == GLFW_PRESS;
                if (f5Down && !f5WasDown) {
                    std::string why;
                    if (app.saveGame(quickSave, &why)) app.notify("saved to Build/quicksave.uksv");
                    else app.notify("save failed: " + why);
                }
                f5WasDown = f5Down;
                const bool f9Down = glfwGetKey(window, GLFW_KEY_F9) == GLFW_PRESS;
                if (f9Down && !f9WasDown) {
                    std::string why;
                    if (app.loadGame(quickSave, &why)) app.notify("loaded Build/quicksave.uksv");
                    else app.notify("load failed: " + why);
                }
                f9WasDown = f9Down;
            }
            if (window && glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) app.requestQuit();

            if (app.sceneGeneration() != loadedGeneration && sceneLoader.empty()) {
                reloadModels();
                renderer.setSceneLights(app.scene().lights);
                // A scene switch brings its own render settings; the debug view is still the one the command line asked
                // for, because it is not part of the scene.
                frameSettings = app.scene().settings;
                frameSettings.wireframe = opt.wireframe;
            }

            app.tick(dt);
            if (app.sceneGeneration() != loadedGeneration && sceneLoader.empty()) {
                reloadModels();
                renderer.setSceneLights(app.scene().lights);
            }
            // The frame's slice of the load. A step runs whole, so the frame pays the budget plus at most one model;
            // `overruns` counts the models that cost more than the whole budget on their own, which is the number worth
            // printing when a load stutters.
            if (opt.loadBudgetMs > 0.0 && !sceneLoader.empty()) {
                const ukiyo::LoadProgress lp = sceneLoader.tick(opt.loadBudgetMs);
                if (lp.done) {
                    std::printf("player: loaded %zu models in %zu steps%s\n", models.size(), lp.stepsRun,
                                lp.overruns > 0 ? " (some overran the budget)" : "");
                }
            }

            // Place and animate the models from the scene's world matrices.
            std::vector<ukiyo::Model*> drawList;
            for (size_t i = 0; i < app.instances().size(); ++i) {
                if (i >= modelForInstance.size() || modelForInstance[i] == SIZE_MAX) continue;
                ukiyo::Model& m = *models[modelForInstance[i]];
                m.placement = app.instances()[i].world;
                ukiyo::applyPlacement(m);
                ukiyo::updateModel(m, float(app.playTime()), cam.eye());
                drawList.push_back(&m);
            }

            if (!device->beginFrame()) continue;
            renderer.render(cam, drawList, float(app.playTime()), uint32_t(capturedSeconds * 60.0f), frameSettings);
            if (hudOn && font.loaded()) {
                hud.setScreen(float(device->backbufferWidth()), float(device->backbufferHeight()));
                drawPlayerHud(hud, app.hudText(renderer.lastDrawCalls(), renderer.lastTriangles()),
                              app.manifest().hudScale);
                renderer.drawHud(hud.vertices(), device->backbufferWidth(), device->backbufferHeight());
            }
            const bool capture = opt.headless && !opt.shot.empty() && capturedSeconds >= opt.seconds;
            device->endFrame(capture);
            if (capture) {
                shot = device->takeCapture();
                break;
            }
            if (opt.headless && opt.seconds > 0.0f && capturedSeconds >= opt.seconds) break;
            if (opt.headless) glfwWaitEventsTimeout(0.001);
        }

        if (!shot.empty() && !opt.shot.empty()) {
            const uint32_t w = device->backbufferWidth();
            const uint32_t h = device->backbufferHeight();
            if (!stbi_write_png(opt.shot.c_str(), int(w), int(h), 4, shot.data(), int(w * 4))) {
                std::fprintf(stderr, "player: could not write %s\n", opt.shot.c_str());
                return 1;
            }
            std::printf("player: wrote %s (%ux%u)\n", opt.shot.c_str(), w, h);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "player: %s\n", e.what());
        return 1;
    }
    app.stop();
    glfwTerminate();
    return 0;
}
