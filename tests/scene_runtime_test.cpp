// The runtime scene reader: the player's half of the .ukscene format.
//
// The editor writes scenes with Qt's JSON classes; the player reads them with this. If the two drift, a game that runs
// fine in the editor fails to boot, so every field the player understands is pinned here against a real file on disk.
// No device, no window: this test runs on CI and locally with a plain g++.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "engine/scene_runtime.h"

using namespace ukiyo;
namespace fs = std::filesystem;

static int failures = 0;
static int checks = 0;

static void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

static bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

struct Sandbox {
    fs::path root;
    Sandbox() {
        const auto tag = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        root = fs::temp_directory_path() / ("ukiyo_scene_runtime_test_" + tag);
        fs::create_directories(root / "Scenes");
    }
    ~Sandbox() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    std::string write(const std::string& rel, const std::string& text) {
        const fs::path p = root / rel;
        fs::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary);
        out << text;
        return p.string();
    }
};

int main() {
    Sandbox sb;

    // A scene that uses everything the runtime knows: two models, the second parented to the first by name, the full
    // rotation, a scale, a LOD ladder, a light, a camera and a couple of render settings.
    const std::string path = sb.write("Scenes/Full.ukscene", R"({
      "format": "ukiyo-scene",
      "version": 2,
      "models": [
        {"path": "assets/Seed-san.vrm", "name": "Seed", "position": [1, 2, 3], "rotation": [0, 90, 0],
         "scale": [2, 2, 2], "lod": [8, 20]},
        {"path": "assets/House.vrm", "name": "House", "parent": "Seed", "position": [0, 0.5, 0],
         "components": {"body": {"dynamic": false, "box": true, "halfExtents": [1, 2, 3]}}},
        {"name": "Marker", "empty": true, "position": [-1, 0, 0]}
      ],
      "lights": [
        {"type": "spot", "position": [0, 4, 0], "direction": [0, -1, 0], "color": [1, 0.8, 0.6],
         "intensity": 3.5, "radius": 12, "angle": 35, "softness": 0.2},
        {"type": "point", "enabled": false}
      ],
      "settings": {"shadows": false, "ssao": 0.7, "tonemap": 3, "celBand": true, "sunDir": [0, -1, 0.2]},
      "gameCamera": {"target": [0, 1.4, 0], "distance": 4.5, "yaw": 0.25, "pitch": 0.15, "fovY": 0.7}
    })");

    RuntimeScene s;
    std::string error;
    check(loadRuntimeScene(path, sb.root.string(), &s, &error), "a full scene loads: " + error);
    check(s.instances.size() == 3, "every model and empty object is an instance");
    check(s.warnings.empty(), "a clean scene produces no warnings");
    check(s.projectRoot == sb.root.string(), "the project root is recorded");
    check(s.sourcePath == path, "the source path is recorded");

    const RuntimeInstance& seed = s.instances[0];
    check(seed.name == "Seed", "the instance keeps its name");
    check(!seed.path.empty() && seed.path.back() == 'm' && seed.path.find(sb.root.string()) == 0,
          "the model path is resolved against the project root");
    check(near(seed.transform.position.x, 1) && near(seed.transform.position.y, 2) && near(seed.transform.position.z, 3),
          "position loads");
    check(near(seed.transform.rotationDeg.y, 90), "rotation loads");
    check(near(seed.transform.scale.x, 2) && near(seed.transform.scale.z, 2), "scale loads");
    check(seed.lod.size() == 2 && near(seed.lod[0], 8) && near(seed.lod[1], 20), "the LOD ladder loads");
    check(seed.parent == -1, "the first instance is a root");

    const RuntimeInstance& house = s.instances[1];
    check(house.parent == 0, "a parent by name resolves to its index");
    check(house.components.body.enabled && !house.components.body.dynamic && house.components.body.box,
          "a body component loads");
    check(near(house.components.body.halfExtents.y, 2), "the body's half extents load");

    check(s.instances[2].empty, "an empty object is marked empty");
    check(s.instances[2].path.empty(), "an empty object has no model path");
    check(s.instances[2].name == "Marker", "the empty object keeps its name");

    check(s.lights.size() == 2, "both lights load");
    check(s.lights[0].type == LightType::Spot, "the spot light's type loads");
    check(near(s.lights[0].intensity, 3.5f) && near(s.lights[0].radius, 12.0f), "spot intensity and radius load");
    check(near(s.lights[0].spotAngleDeg, 35.0f), "the spot cone loads");
    check(near(s.lights[0].color.g, 0.8f), "the light colour loads");
    check(s.lights[0].enabled, "a light defaults to enabled");
    check(!s.lights[1].enabled, "an explicit enabled:false loads");

    check(!s.settings.shadows, "a render setting out of the file wins over the default");
    check(near(s.settings.ssao, 0.7f), "a float render setting loads");
    check(s.settings.tonemap == 3, "an int render setting loads");
    check(s.settings.celBand, "celBand is a switch and loads as one");
    check(near(s.settings.sunDir.z, 0.2f), "the sun direction loads");
    check(s.settings.grid, "a setting the file omits keeps its default");

    check(near(s.cameraDistance, 4.5f) && near(s.cameraYaw, 0.25f) && near(s.cameraPitch, 0.15f),
          "the game camera loads");
    check(near(s.cameraTarget.y, 1.4f) && near(s.cameraFovY, 0.7f), "the camera target and fov load");

    // A legacy scene: one model under "model", position and yawDeg only. Every scene saved before the instance list
    // existed looks like this, and the player must still boot it.
    const std::string legacy = sb.write("Scenes/Legacy.ukscene", R"({
      "format": "ukiyo-scene", "version": 1,
      "model": "assets/Seed-san.vrm",
      "nodes": [{"name": "hair", "translation": [0, 0, 0]}]
    })");
    RuntimeScene l;
    check(loadRuntimeScene(legacy, sb.root.string(), &l, &error), "a legacy single-model scene loads: " + error);
    check(l.instances.size() == 1, "the legacy model becomes one instance");
    check(l.instances[0].path == sb.root.string() + "/assets/Seed-san.vrm", "the legacy path is resolved");
    check(l.instances[0].name == "Seed-san.vrm", "an unnamed instance takes its file name");
    check(near(l.instances[0].transform.position.y, 0.0f), "the legacy default position is the origin");

    // yawDeg is the old form of rotation; a file carrying both is not ambiguous, the full rotation wins.
    const std::string both = sb.write("Scenes/Both.ukscene", R"({
      "format": "ukiyo-scene",
      "models": [{"path": "a.vrm", "position": [0, 1, 0], "yawDeg": 30, "rotation": [10, 40, 0]}]
    })");
    RuntimeScene b;
    check(loadRuntimeScene(both, sb.root.string(), &b, &error), "position and yaw with rotation loads");
    check(near(b.instances[0].transform.rotationDeg.y, 40.0f), "rotation overrides yawDeg");
    check(near(b.instances[0].transform.rotationDeg.x, 10.0f) && near(b.instances[0].transform.rotationDeg.z, 0.0f),
          "the other rotation axes come from rotation");

    // A file that carries only yawDeg still gets its yaw.
    const std::string yaw = sb.write("Scenes/Yaw.ukscene", R"({
      "format": "ukiyo-scene", "models": [{"path": "a.vrm", "yawDeg": 30}]
    })");
    RuntimeScene y;
    check(loadRuntimeScene(yaw, sb.root.string(), &y, &error), "a yaw-only instance loads");
    check(near(y.instances[0].transform.rotationDeg.y, 30.0f), "yawDeg loads into the y rotation");

    // Parenting by index, and a forward reference by name: the name is known by the time the loader repairs links, and
    // an unknown one is a warning with the instance left at the root.
    const std::string links = sb.write("Scenes/Links.ukscene", R"({
      "format": "ukiyo-scene",
      "models": [
        {"path": "a.vrm", "name": "A"},
        {"path": "b.vrm", "name": "B", "parent": 0},
        {"path": "c.vrm", "name": "C", "parent": "Ghost"},
        {"path": "d.vrm", "name": "D", "parent": "E"},
        {"path": "e.vrm", "name": "E", "parent": "D"}
      ]
    })");
    RuntimeScene k;
    check(loadRuntimeScene(links, sb.root.string(), &k, &error), "a scene with links loads");
    check(k.instances[1].parent == 0, "a parent by index loads");
    check(k.instances[2].parent == -1, "an unknown parent leaves the instance at the root");
    check(!k.warnings.empty(), "an unknown parent is reported");
    // D and E name each other. At least one of the two links must be dropped, and afterwards the graph must be acyclic.
    check(k.instances[3].parent == -1 || k.instances[4].parent == -1, "a two-instance cycle is broken");
    check(!(k.instances[3].parent == 4 && k.instances[4].parent == 3), "the cycle does not survive the load");
    check(k.instances.size() == 5, "a broken link does not drop the instance");

    // A prefab reference cannot be flattened by the player yet. It is reported and skipped to an empty instance rather
    // than crashing, so a scene with one still boots.
    const std::string prefab = sb.write("Scenes/Prefab.ukscene", R"({
      "format": "ukiyo-scene",
      "models": [{"prefab": "Prefabs/Lamp.uprefab", "name": "Lamp"}, {"path": "a.vrm", "name": "A"}]
    })");
    RuntimeScene p;
    check(loadRuntimeScene(prefab, sb.root.string(), &p, &error), "a scene with a prefab reference loads");
    check(p.instances.size() == 2, "the prefab keeps its place in the list");
    check(!p.warnings.empty(), "the prefab reference is reported");
    check(p.instances[0].path.empty(), "a prefab reference has no model to load");

    // Every component the scene format can carry, read into the systems' own structs.
    const std::string comps = sb.write("Scenes/Components.ukscene", R"({
      "format": "ukiyo-scene",
      "models": [{"path": "a.vrm", "components": {
        "agent": {"maxSpeed": 2.5},
        "audio": {"sound": "assets/step.wav", "loop": false, "playOnStart": false, "volume": 0.4},
        "anim": {"idle": "idle", "walk": "walk", "run": "run", "walkSpeed": 1.2, "runSpeed": 6},
        "script": {"path": "Scripts/player.lua"},
        "emitter": {"rate": 12, "lifetime": 2, "sizeStart": 0.3, "colorEnd": [1, 0, 0, 0]},
        "action": {"kind": "pingpong", "speed": 1.5, "amplitude": 0.5}
      }}]
    })");
    RuntimeScene c;
    check(loadRuntimeScene(comps, sb.root.string(), &c, &error), "components load");
    const SystemsComponents& sc = c.instances[0].components;
    check(sc.agent.enabled && near(sc.agent.maxSpeed, 2.5f), "agent loads");
    check(sc.audio.enabled && !sc.audio.loop && !sc.audio.playOnStart && near(sc.audio.volume, 0.4f), "audio loads");
    check(sc.audio.sound == "assets/step.wav", "the audio clip loads");
    check(sc.anim.enabled && sc.anim.runClip == "run" && near(sc.anim.runSpeed, 6.0f), "anim loads");
    check(sc.script.enabled && sc.script.path == "Scripts/player.lua", "script loads");
    check(sc.emitter.enabled && near(sc.emitter.settings.rate, 12.0f), "emitter loads");
    check(near(sc.emitter.settings.colorEnd.x, 1.0f), "the emitter's end colour loads");
    check(sc.action.enabled && sc.action.pingPong && near(sc.action.amplitude, 0.5f), "action loads");

    // The capsule character (item 4) is a component like any other: it loads from the scene and keeps its defaults.
    const std::string chr = sb.write("Scenes/Character.ukscene", R"({
      "format": "ukiyo-scene",
      "models": [{"path": "a.vrm", "components": {"character": {"radius": 0.35, "height": 1.7, "speed": 5}}}]
    })");
    RuntimeScene ch;
    check(loadRuntimeScene(chr, sb.root.string(), &ch, &error), "a character component loads");
    check(ch.instances[0].components.character.enabled, "the character is enabled");
    check(std::fabs(ch.instances[0].components.character.radius - 0.35f) < 1e-5f, "the character's radius loads");
    check(std::fabs(ch.instances[0].components.character.height - 1.7f) < 1e-5f, "the character's height loads");
    check(std::fabs(ch.instances[0].components.character.speed - 5.0f) < 1e-5f, "the character's speed loads");
    check(std::fabs(ch.instances[0].components.character.stepHeight - 0.4f) < 1e-5f, "an omitted field keeps its default");

    // Refusals: each one is a file the player must reject with a readable reason rather than boot half a game.
    RuntimeScene bad;
    error.clear();
    check(!loadRuntimeScene(sb.root.string() + "/Scenes/Missing.ukscene", sb.root.string(), &bad, &error),
          "a missing file fails");
    check(error.find("Missing.ukscene") != std::string::npos, "the error names the missing file");

    const std::string notScene = sb.write("Scenes/NotScene.ukscene", R"({"format":"ukiyo-build","version":1,"scenes":["a"]})");
    check(!loadRuntimeScene(notScene, sb.root.string(), &bad, &error), "a build manifest is not a scene");

    const std::string noInstances = sb.write("Scenes/Empty.ukscene", R"({"format":"ukiyo-scene","models":[]})");
    check(!loadRuntimeScene(noInstances, sb.root.string(), &bad, &error), "a scene with no instances fails");

    const std::string modelsObject = sb.write("Scenes/ModelsObject.ukscene", R"({"format":"ukiyo-scene","models":{}})");
    check(!loadRuntimeScene(modelsObject, sb.root.string(), &bad, &error), "models that are not an array fail");

    const std::string broken = sb.write("Scenes/Broken.ukscene", "{");
    check(!loadRuntimeScene(broken, sb.root.string(), &bad, &error), "broken JSON fails");
    check(error.find("Broken.ukscene") != std::string::npos, "the error names the broken file");
    check(!loadRuntimeScene(path, sb.root.string(), nullptr, &error), "a null output is refused");

    // An absolute path survives untouched; the player accepts a scene given by its absolute path.
    const std::string absolute = sb.write("Scenes/Absolute.ukscene", R"({
      "format": "ukiyo-scene", "models": [{"path": "/opt/assets/a.vrm"}]
    })");
    RuntimeScene a;
    check(loadRuntimeScene(absolute, "/somewhere/else", &a, &error), "an absolute scene loads");
    check(a.instances[0].path == "/opt/assets/a.vrm", "an absolute model path is not prefixed");

    std::printf("scene_runtime_test: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
