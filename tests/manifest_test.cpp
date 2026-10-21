// The build manifest: what the editor's Build page writes and the player reads.
//
// The two halves live in different programs, so the format is the contract. This test pins it: a manifest the writer
// produces always parses, a manifest that violates the contract is refused with a reason, and the defaults the player
// relies on (first scene, clamps) hold. It runs with no device and no window.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "engine/build_manifest.h"

using namespace ukiyo;

static int failures = 0;
static int checks = 0;

static void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

static bool parse(const std::string& json, BuildManifest* out, std::string* error) {
    *error = "";
    const bool ok = parseBuildManifest(json, out, error);
    if (!ok) check(!error->empty(), "a refusal carries a reason");
    return ok;
}

int main() {
    BuildManifest m;
    m.name = "Seed Garden";
    m.version = "0.4.2";
    m.scenes = {"Scenes/Main.ukscene", "Scenes/House.ukscene", "Scenes/Field.ukscene"};
    m.startScene = "Scenes/House.ukscene";
    m.font = "assets/fonts/hud.ttf";
    m.hudScale = 1.5f;
    m.window.width = 1920;
    m.window.height = 1080;
    m.window.fullscreen = true;
    m.window.vsync = false;

    const std::string json = buildManifestJson(m);
    BuildManifest back;
    std::string error;
    check(parse(json, &back, &error), "the writer's output parses");
    check(back.name == "Seed Garden", "round trip: name");
    check(back.version == "0.4.2", "round trip: versionName");
    check(back.scenes == m.scenes, "round trip: scenes keep their order");
    check(back.startScene == m.startScene, "round trip: start scene");
    check(back.font == m.font, "round trip: font");
    check(back.hudScale == 1.5f, "round trip: hud scale");
    check(back.window.width == 1920 && back.window.height == 1080, "round trip: window size");
    check(back.window.fullscreen && !back.window.vsync, "round trip: fullscreen and vsync");
    check(firstScene(back) == "Scenes/House.ukscene", "firstScene uses the start scene");
    check(json.find("\"format\": \"ukiyo-build\"") != std::string::npos, "the format tag is written first-ish");

    // Every manifest the writer produces parses, including the empty-name and boundary cases. This is the property the
    // editor relies on when it validates before saving; checking it over a set of shapes catches a writer that starts
    // emitting something the parser drops.
    const std::vector<std::string> names = {"", "a", "Seed Garden", "日本語のゲーム", "quote\"and\\slash"};
    const std::vector<float> scales = {0.25f, 1.0f, 3.0f, 8.0f};
    for (const std::string& n : names) {
        for (float s : scales) {
            BuildManifest a;
            a.name = n;
            a.hudScale = s;
            a.scenes = {"a.ukscene"};
            BuildManifest b;
            std::string e;
            check(parse(buildManifestJson(a), &b, &e) && b.name == n && std::abs(b.hudScale - s) < 1e-6f,
                  "writer output always parses: " + n);
        }
    }

    // A start scene of "" means "the first scene".
    BuildManifest first;
    first.scenes = {"one.ukscene", "two.ukscene"};
    check(firstScene(first) == "one.ukscene", "firstScene falls back to the front");
    check(firstScene(BuildManifest()) == "", "no scenes, no first scene");

    // Refusals. Each is a mistake an author can actually make.
    BuildManifest out;
    check(!parse(R"({"format":"ukiyo-scene","version":1,"scenes":["a"]})", &out, &error), "a scene is not a manifest");
    check(!parse(R"({"format":"ukiyo-build","version":2,"scenes":["a"]})", &out, &error), "a newer version is refused");
    check(!parse(R"({"format":"ukiyo-build","version":0,"scenes":["a"]})", &out, &error), "version 0 is refused");
    check(!parse(R"({"scenes":["a"]})", &out, &error), "a missing format is refused");
    check(!parse(R"({"format":"ukiyo-build","version":1})", &out, &error), "no scenes is refused");
    check(!parse(R"({"format":"ukiyo-build","version":1,"scenes":[]})", &out, &error), "an empty scene list is refused");
    check(!parse(R"({"format":"ukiyo-build","version":1,"scenes":"a"})", &out, &error), "a string scene list is refused");
    check(!parse(R"({"format":"ukiyo-build","version":1,"scenes":["a"],"startScene":"b"})", &out, &error),
          "a start scene outside the list is refused");
    check(!parse("{", &out, &error), "broken JSON is refused");
    check(!parse("[1,2,3]", &out, &error), "a JSON array is refused");
    error = "untouched";
    check(!parseBuildManifest("{}", &out, nullptr), "a null error pointer is allowed");

    // Older files: the Build page used to write only productName and scenes, as bare strings.
    check(parse(R"({"format":"ukiyo-build","version":1,"productName":"Old Game","scenes":["a.ukscene"]})", &out, &error),
          "an old Build page file still loads");
    check(out.name == "Old Game", "productName is read as the name");
    check(out.window.width == 1280 && out.window.height == 720, "an absent window block uses 1280x720");
    check(out.hudScale == 1.0f, "an absent hudScale is 1");

    // Newer scenes: objects with a path, which is what a scene entry with a name would look like.
    check(parse(R"({"format":"ukiyo-build","version":1,"scenes":[{"path":"a.ukscene"},{"path":"b.ukscene"}]})", &out,
                &error),
          "scene objects load");
    check(out.scenes.size() == 2 && out.scenes[1] == "b.ukscene", "scene objects keep their paths");

    // Clamps: a hand-edited manifest cannot ask for a window the driver will refuse or a HUD that covers the screen.
    check(parse(R"({"format":"ukiyo-build","version":1,"scenes":["a"],"window":{"width":10,"height":99999}})", &out,
                &error),
          "absurd window sizes still parse");
    check(out.window.width == 320, "window width is clamped up");
    check(out.window.height == 4320, "window height is clamped down");
    check(parse(R"({"format":"ukiyo-build","version":1,"scenes":["a"],"hudScale":40})", &out, &error), "hudScale parses");
    check(out.hudScale == 8.0f, "hudScale is clamped to 8");
    check(parse(R"({"format":"ukiyo-build","version":1,"scenes":["a"],"hudScale":0.01})", &out, &error), "hudScale parses");
    check(out.hudScale == 0.25f, "hudScale is clamped to 0.25");

    // Unknown fields are ignored, so a newer editor can add one without breaking a shipped player.
    check(parse(R"({"format":"ukiyo-build","version":1,"scenes":["a"],"achievements":["first"]})", &out, &error),
          "unknown fields are ignored");
    check(parse(R"({"format":"ukiyo-build","version":1,"scenes":["a"],"name":"X"})", &out, &error) && out.name == "X",
          "name wins over productName");

    // loadBuildManifest reports the path, which is what the player prints when a build is misconfigured.
    BuildManifest missing;
    std::string why;
    check(!loadBuildManifest("/nonexistent/Build/build.json", &missing, &why), "a missing file fails");
    check(why.find("/nonexistent/Build/build.json") != std::string::npos, "the error names the path");

    std::printf("manifest_test: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
