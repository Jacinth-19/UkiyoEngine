// The player runtime, without a window or a GPU.
//
// PlayerApp is what a shipped game runs; main.cpp only opens a window around it. This test boots a project from a
// manifest the editor would write, ticks the world, and checks the things a player must get right: the start scene, the
// instance hierarchy, bodies falling, HUD strings, and a script's scene switch. It needs no device, so CI runs it.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
#include <string>

#include <GLFW/glfw3.h>

#include "engine/build_manifest.h"
#include "player/player_app.h"

using namespace ukiyo;
namespace fs = std::filesystem;

static int failures = 0;
static int checks = 0;

static void check(bool ok, const std::string& what, const std::string& detail = std::string()) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s%s%s\n", what.c_str(), detail.empty() ? "" : " -- ", detail.c_str());
    }
}

static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

struct Sandbox {
    fs::path root;
    Sandbox() {
        const auto tag = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        root = fs::temp_directory_path() / ("ukiyo_player_test_" + tag);
        fs::create_directories(root);
    }
    ~Sandbox() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    void write(const std::string& rel, const std::string& text) {
        const fs::path p = root / rel;
        fs::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary);
        out << text;
    }
    std::string str() const { return root.string(); }
};

static BuildManifest makeManifest(const std::vector<std::string>& scenes, const std::string& start) {
    BuildManifest m;
    m.name = "Player Test";
    m.version = "2.1.0";
    m.scenes = scenes;
    m.startScene = start;
    return m;
}

int main() {
    Sandbox sb;

    // Scene A: a floor, a ball that falls onto it, a hat parented to the ball, and a script that leaves for scene B.
    sb.write("Scenes/A.ukscene", R"({
      "format": "ukiyo-scene",
      "models": [
        {"name": "Floor", "empty": true, "position": [0, -0.5, 0],
         "components": {"body": {"dynamic": false, "box": true, "halfExtents": [10, 0.5, 10]}}},
        {"name": "Ball", "empty": true, "position": [0, 4, 0],
         "components": {"body": {"dynamic": true, "radius": 0.5, "mass": 1}}},
        {"name": "Hat", "empty": true, "parent": "Ball", "position": [0, 1, 0]}
      ],
      "gameCamera": {"target": [0, 1, 0], "distance": 8, "yaw": 0.3, "pitch": 0.2, "fovY": 0.8}
    })");

    sb.write("Scenes/B.ukscene", R"({
      "format": "ukiyo-scene",
      "models": [{"name": "Other", "empty": true, "position": [3, 0, 0]}]
    })");

    sb.write("Scripts/leave.lua", R"(
local t = 0
function onUpdate(dt)
  t = t + dt
  if t > 0.05 then ukiyo.loadScene("Scenes/B.ukscene") end
end
)");

    // The manifest names scene B as the start scene even though A comes first: the start scene must win.
    const BuildManifest manifest = makeManifest({"Scenes/A.ukscene", "Scenes/B.ukscene"}, "Scenes/B.ukscene");
    sb.write("Build/build.json", buildManifestJson(manifest));

    PlayerApp app(false);
    std::string error;
    check(app.start("Build/build.json", sb.str(), &error), "the player boots: " + error);
    check(app.running(), "a started player runs");
    check(app.scene().sourcePath.find("B.ukscene") != std::string::npos, "the start scene wins over the first scene");
    check(app.instances().size() == 1 && app.instances()[0].name == "Other", "the start scene's instances load");
    check(app.manifest().name == "Player Test" && app.manifest().version == "2.1.0", "the manifest loads");
    check(app.title() == "Player Test 2.1.0", "the title is the product name and version");
    check(app.sceneGeneration() > 0, "boot bumps the scene generation");
    app.stop();

    // Scene A, chosen by dropping the start scene: bodies, hierarchy, HUD.
    sb.write("Build/build.json", buildManifestJson(makeManifest({"Scenes/A.ukscene", "Scenes/B.ukscene"}, "")));
    PlayerApp world(false);
    check(world.start("Build/build.json", sb.str(), &error), "the player boots the first scene: " + error);
    check(world.instances().size() == 3, "three instances load");
    check(world.instances()[1].name == "Ball", "instances keep their order and names");
    check(world.instances()[2].parent == 1, "the hat is parented to the ball");
    check(near(world.instances()[2].world[3].y, 5.0f), "the child's world matrix includes the parent's position");
    check(near(world.instances()[1].world[3].y, 4.0f), "the ball's world matrix is its own position");
    check(near(world.scene().cameraDistance, 8.0f), "the scene's camera loads");
    check(world.scriptsRunning() == 0, "no script, no script running");

    // One second of falling. The ball starts at y=4 and must be lower and moving; the floor must not move at all.
    const float floorStart = world.instances()[0].world[3].y;
    for (int i = 0; i < 60; ++i) world.tick(1.0f / 60.0f);
    const float ballY = world.instances()[1].world[3].y;
    check(ballY < 3.9f, "the dynamic body falls under gravity");
    check(ballY > -10.0f, "the ball is still in the world after a second");
    check(near(world.instances()[0].world[3].y, floorStart), "a static body does not move");
    check(near(world.instances()[2].world[3].y, ballY + 1.0f), "the child follows the falling parent exactly");
    check(world.playTime() > 0.9 && world.playTime() < 1.1, "the clock advances with the ticks");
    check(near(world.scene().cameraDistance, 8.0f), "the camera survives the ticks");

    // HUD strings. The layout is tested elsewhere; what matters here is that the runtime describes itself.
    world.reportFrame(1.0f / 50.0f);
    check(world.fps() > 40.0f && world.fps() < 60.0f, "the frame rate is measured");
    check(near(world.frameMs(), 20.0f, 0.5f), "the frame time is measured");
    const PlayerApp::HudText text = world.hudText(12, 3400);
    check(text.title == "Player Test 2.1.0", "the HUD title is the build");
    check(text.stats.find("fps") != std::string::npos, "the stats line carries the frame rate");
    check(text.stats.find("12 draws") != std::string::npos, "the stats line carries the draw count");
    check(text.stats.find("3.4k tris") != std::string::npos, "the stats line carries the triangle count");
    check(!text.hint.empty(), "the controls line is present");
    check(text.warning.empty(), "a clean scene has no warning");

    // Keys are accepted without a window and do not disturb a scene with no script.
    world.setKey("W", true);
    world.setKey("W", false);
    world.setKey("", true);
    world.tick(1.0f / 60.0f);

    // A script switching scenes: the generation moves, the instances are the new scene's, and the clock restarts.
    const uint32_t before = world.sceneGeneration();
    world.stop();
    sb.write("Scenes/Scripted.ukscene", R"({
      "format": "ukiyo-scene",
      "models": [
        {"name": "Runner", "empty": true,
         "components": {"script": {"path": "Scripts/leave.lua"}}}
      ]
    })");
    sb.write("Build/build.json",
             buildManifestJson(makeManifest({"Scenes/Scripted.ukscene", "Scenes/B.ukscene"}, "")));
    PlayerApp scripted(false);
    check(scripted.start("Build/build.json", sb.str(), &error), "a scene with a script boots: " + error);
    check(scripted.scriptsRunning() == 1, "the script is running");
    for (int i = 0; i < 20 && scripted.sceneGeneration() == 1; ++i) scripted.tick(1.0f / 60.0f);
    check(scripted.sceneGeneration() > 1, "the script's loadScene replaced the scene");
    check(scripted.scene().sourcePath.find("B.ukscene") != std::string::npos, "the requested scene is the one loaded");
    check(scripted.instances().size() == 1 && scripted.instances()[0].name == "Other", "the new scene's instances load");
    check(scripted.playTime() >= 0.0 && scripted.playTime() < 0.1, "the scene switch restarts the clock");
    check(scripted.sceneGeneration() != before, "the generation keeps moving");

    // Save and load: a quicksave captures where everything is, and loading it puts a moved body back. The checksum and
    // the scene match are enforced, so a save from another scene or a corrupted file is refused.
    {
        sb.write("Build/save.json", buildManifestJson(makeManifest({"Scenes/A.ukscene", "Scenes/B.ukscene"}, "")));
        PlayerApp game(false);
        check(game.start("Build/save.json", sb.str(), &error), "the save scene boots: " + error);
        size_t ball = 0;
        for (size_t i = 0; i < game.instances().size(); ++i)
            if (game.instances()[i].name == "Ball") ball = i;
        check(game.instances()[ball].name == "Ball", "the save scene has the ball");
        for (int i = 0; i < 30; ++i) game.tick(1.0f / 60.0f);
        const glm::vec3 before = game.instances()[ball].local.position;
        const std::string savePath = (sb.root / "Build" / "quicksave.uksv").string();
        check(game.saveGame(savePath, &error), "a save is written: " + error);
        // Move the ball, then load: the saved position must win.
        for (int i = 0; i < 10; ++i) game.tick(1.0f / 60.0f);
        check(glm::length(game.instances()[ball].local.position - before) > 0.01f, "the ball moved before the load");
        check(game.loadGame(savePath, &error), "the save loads back: " + error);
        check(glm::length(game.instances()[ball].local.position - before) < 1e-4f, "the load restores the saved position");
        check(game.playTime() >= 0.4 && game.playTime() <= 0.6, "the load restores the play time");

        // A save from a different scene is refused rather than applied to the wrong geometry.
        sb.write("Build/other.json", buildManifestJson(makeManifest({"Scenes/B.ukscene"}, "")));
        PlayerApp other(false);
        check(other.start("Build/other.json", sb.str(), &error), "the other scene boots");
        check(!other.loadGame(savePath, &error), "a save from another scene is refused");

        // A corrupted byte fails the checksum and nothing is applied.
        std::vector<uint8_t> bytes;
        {
            std::ifstream in(savePath, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        check(!bytes.empty(), "the save file has content");
        bytes[bytes.size() / 2] ^= 0xFF;
        const std::string brokenSave = (sb.root / "Build" / "broken.uksv").string();
        {
            std::ofstream out(brokenSave, std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        const glm::vec3 keep = game.instances()[ball].local.position;
        check(!game.loadGame(brokenSave, &error), "a corrupted save is refused");
        check(glm::length(game.instances()[ball].local.position - keep) < 1e-6f, "a refused load leaves the game untouched");

        // The notification is what the HUD shows for a save result.
        game.notify("saved");
        check(game.hudText(0, 0).warning == "saved", "the notification reaches the HUD");
        game.notify("");
        check(game.hudText(0, 0).warning.empty(), "clearing the notification clears the HUD warning");
    }

    // A tilted instance (the gizmo's X and Z rings): the scene's rotation reaches the entity and the collider, the
    // crate that lands on the ramp slides down it, and a save carries the whole rotation back.
    {
        sb.write("Scenes/Tilted.ukscene", R"({
          "format": "ukiyo-scene",
          "models": [
            {"name": "Ramp", "empty": true, "position": [0, 0, 0], "rotation": [0, 0, 25],
             "components": {"body": {"dynamic": false, "box": true, "halfExtents": [4, 0.1, 4]}}},
            {"name": "Ground", "empty": true, "position": [0, -4, 0],
             "components": {"body": {"dynamic": false, "box": true, "halfExtents": [12, 0.5, 12]}}},
            {"name": "Crate", "empty": true, "position": [0, 3, 0],
             "components": {"body": {"dynamic": true, "box": true, "halfExtents": [0.4, 0.4, 0.4], "mass": 5}}}
          ]
        })");
        sb.write("Build/tilted.json", buildManifestJson(makeManifest({"Scenes/Tilted.ukscene"}, "")));
        PlayerApp tilt(false);
        check(tilt.start("Build/tilted.json", sb.str(), &error), "the tilted scene boots: " + error);
        check(tilt.instances().size() == 3, "the tilted scene has its three instances");
        size_t ramp = 0, crate = 0;
        for (size_t i = 0; i < tilt.instances().size(); ++i) {
            if (tilt.instances()[i].name == "Ramp") ramp = i;
            if (tilt.instances()[i].name == "Crate") crate = i;
        }
        check(near(tilt.instances()[ramp].local.rotationDeg.z, 25.0f), "the scene's rotation loads into the instance");
        for (int i = 0; i < 240; ++i) tilt.tick(1.0f / 60.0f);
        // A positive turn about Z lifts the +x end, so downhill is -x; the crate leaves the ramp and lands on the floor.
        check(tilt.instances()[crate].local.position.x < -0.5f,
              "the crate slides down the ramp the scene tilted (x = " + std::to_string(tilt.instances()[crate].local.position.x) + ")");
        const glm::vec3 turned = tilt.instances()[crate].local.rotationDeg;
        check(std::fabs(turned.z) > 1.0f, "the crate is drawn turned, the way physics left it",
              std::to_string(turned.z));
        const std::string tiltSave = (sb.root / "Build" / "tilt.uksv").string();
        check(tilt.saveGame(tiltSave, &error), "the tilted scene saves: " + error);
        for (int i = 0; i < 30; ++i) tilt.tick(1.0f / 60.0f);
        check(tilt.loadGame(tiltSave, &error), "the tilted save loads: " + error);
        check(glm::length(tilt.instances()[crate].local.rotationDeg - turned) < 1e-3f,
              "the load restores the crate's whole rotation, not only its yaw");
        check(near(tilt.instances()[ramp].local.rotationDeg.z, 25.0f), "the load keeps the ramp's tilt");
    }

    // The capsule character (item 4): a character component makes an instance walkable. W walks it forward along the
    // camera's yaw, Space jumps, and the instance's world matrix follows the capsule.
    {
        sb.write("Scenes/Char.ukscene", R"({
          "format": "ukiyo-scene",
          "models": [
            {"name": "CharGround", "empty": true, "position": [0, -0.5, 0],
             "components": {"body": {"dynamic": false, "box": true, "halfExtents": [12, 0.5, 12]}}},
            {"name": "Hero", "empty": true, "position": [0, 0, 0],
             "components": {"character": {"radius": 0.3, "height": 1.8, "speed": 3, "jump": 5}}}
          ],
          "gameCamera": {"yaw": 0, "distance": 6}
        })");
        sb.write("Build/char.json", buildManifestJson(makeManifest({"Scenes/Char.ukscene"}, "")));
        PlayerApp hero(false);
        check(hero.start("Build/char.json", sb.str(), &error), "the character scene boots: " + error);
        check(hero.instances().size() == 2, "the character scene has both instances");
        for (int i = 0; i < 30; ++i) hero.tick(1.0f / 60.0f);   // settle onto the ground
        const float z0 = hero.instances()[1].world[3].z;
        hero.setKey("W", true);
        for (int i = 0; i < 60; ++i) hero.tick(1.0f / 60.0f);
        hero.setKey("W", false);
        const float z1 = hero.instances()[1].world[3].z;
        check(z1 < z0 - 2.0f, "W walks the character forward (dz = " + std::to_string(z1 - z0) + ")");
        check(hero.instances()[1].world[3].y < 0.2f && hero.instances()[1].world[3].y > -0.2f,
              "the character stays on the ground while walking");

        // Jump: Space leaves the ground and lands again.
        float peak = 0.0f;
        bool airborne = false;
        hero.setKey("Space", true);
        for (int i = 0; i < 20; ++i) {
            hero.tick(1.0f / 60.0f);
            peak = std::max(peak, hero.instances()[1].world[3].y);
            CharacterState st;
            if (hero.world()->characterState("Hero", &st) && !st.grounded) airborne = true;
        }
        hero.setKey("Space", false);
        check(airborne, "Space jumps the character off the ground");
        check(peak > 0.3f, "the jump rises (peak " + std::to_string(peak) + " m)");
        for (int i = 0; i < 120; ++i) hero.tick(1.0f / 60.0f);
        CharacterState landed;
        check(hero.world()->characterState("Hero", &landed) && landed.grounded, "the character lands again");
    }

    // Quitting and stopping.
    scripted.requestQuit();
    check(!scripted.running(), "requestQuit stops the loop");
    scripted.stop();
    check(!scripted.running(), "stop leaves the player stopped");
    check(scripted.world() == nullptr, "stop releases the world");

    // Failures: a missing manifest and a start scene outside the list. Both must return a reason, not a half-started
    // player, because main.cpp prints it and exits.
    PlayerApp broken(false);
    std::string why;
    check(!broken.start("Build/nope.json", sb.str(), &why), "a missing manifest fails");
    check(why.find("nope.json") != std::string::npos, "the error names the manifest");
    check(!broken.running(), "a failed start is not running");

    sb.write("Build/badstart.json",
             buildManifestJson(makeManifest({"Scenes/A.ukscene"}, "Scenes/Nowhere.ukscene")));
    PlayerApp badStart(false);
    check(!badStart.start("Build/badstart.json", sb.str(), &why), "a start scene outside the list fails");
    check(!badStart.running(), "the failed start stayed stopped");

    // A scene file the manifest names but the project does not have.
    sb.write("Build/missingScene.json", buildManifestJson(makeManifest({"Scenes/Gone.ukscene"}, "")));
    PlayerApp gone(false);
    check(!gone.start("Build/missingScene.json", sb.str(), &why), "a missing scene fails the boot");
    check(why.find("Gone.ukscene") != std::string::npos, "the error names the scene");

    // Key names: the player hands the input system the same text the editor's input asset uses.
    check(glfwKeyNameToQt(GLFW_KEY_SPACE, nullptr) == "Space", "space maps to Space");
    check(glfwKeyNameToQt(GLFW_KEY_LEFT, nullptr) == "Left", "an arrow key maps to Left");
    check(glfwKeyNameToQt(GLFW_KEY_ESCAPE, nullptr) == "Esc", "escape maps to Esc");
    check(glfwKeyNameToQt(GLFW_KEY_F5, nullptr) == "F5", "a function key maps to F5");
    check(glfwKeyNameToQt(0, "w") == "W", "a printable key is upper-cased");
    check(glfwKeyNameToQt(0, "7") == "7", "a digit is unchanged");
    check(glfwKeyNameToQt(0, nullptr).empty(), "an unknown key has no name");

    std::printf("player_test: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
