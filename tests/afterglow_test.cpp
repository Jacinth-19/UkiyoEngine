// Afterglow's contract: the dusk village a player installs, checked without a device.
//
// Lantern Run's two tests hold *their* project's contract (they hardcode it on purpose — the sample must not drift).
// This is the second game's: the manifest, the twenty-four instances and fifteen lights of Scenes/Village.ukscene,
// the ten relights the script and the scene must agree on, the locales a player sees, and the audio the music
// pipeline wrote. Every claim docs/GAME.md makes about Afterglow appears below as a check, and CI runs this on
// Linux, Windows and the two phone ABIs' compilation (game_data_test covers the phone readers; this covers the
// game itself).
//
//   afterglow_test [project directory]        (default: games/afterglow)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "engine/build_manifest.h"
#include "engine/localisation.h"
#include "engine/scene_runtime.h"

namespace {

namespace fs = std::filesystem;

int g_pass = 0, g_fail = 0;

void check(bool ok, const char* name, const std::string& detail = "") {
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : "  -- ", detail.c_str());
    ok ? ++g_pass : ++g_fail;
}

std::string readText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// A WAV header check: RIFF/WAVE, one channel, 48 kHz, and a duration the script's music expects to loop.
bool wavInfo(const std::string& path, uint32_t* rate, uint16_t* channels, double* seconds) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char riff[12];
    f.read(riff, 12);
    if (!f || std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) return false;
    uint32_t rate32 = 0;
    uint16_t channels16 = 0, bits = 0;
    uint32_t dataSize = 0;
    while (f && !(rate32 && dataSize)) {
        char id[4];
        uint32_t size = 0;
        f.read(id, 4);
        f.read(reinterpret_cast<char*>(&size), 4);
        if (!f) break;
        if (std::memcmp(id, "fmt ", 4) == 0 && size >= 16) {
            // PCM fmt: format(2) channels(2) rate(4) byteRate(4) blockAlign(2) bits(2) — skip the two the
            // duration does not need instead of reading them as bits, which reports every file as 2-bit.
            uint16_t format = 0;
            f.read(reinterpret_cast<char*>(&format), 2);
            f.read(reinterpret_cast<char*>(&channels16), 2);
            f.read(reinterpret_cast<char*>(&rate32), 4);
            f.ignore(6);
            f.read(reinterpret_cast<char*>(&bits), 2);
            if (size > 16) f.ignore(size - 16);
            if (format != 1) return false;  // PCM: what tools/audio_gen.py writes
        } else if (std::memcmp(id, "data", 4) == 0) {
            dataSize = size;
            f.ignore(size);
        } else {
            f.ignore(size + (size & 1));
        }
    }
    if (!rate32 || !channels16 || !dataSize || bits != 16) return false;
    if (rate) *rate = rate32;
    if (channels) *channels = channels16;
    if (seconds) *seconds = double(dataSize) / (double(rate32) * channels16 * (bits / 8));
    return true;
}

bool has(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

} // namespace

int main(int argc, char** argv) {
    const std::string project = argc > 1 ? argv[1] : "games/afterglow";
    std::printf("afterglow_test: %s\n", project.c_str());

    // --- The manifest ---------------------------------------------------------
    ukiyo::BuildManifest manifest;
    std::string error;
    check(ukiyo::loadBuildManifest(project + "/Build/build.json", &manifest, &error), "the build manifest loads",
          error);
    check(manifest.name == "Afterglow", "the build is called Afterglow", manifest.name);
    check(!ukiyo::firstScene(manifest).empty(), "it names a start scene", ukiyo::firstScene(manifest));
    check(!manifest.font.empty() && fs::exists(project + "/" + manifest.font), "its HUD font is on disk",
          manifest.font);

    // --- The scene ------------------------------------------------------------
    ukiyo::RuntimeScene scene;
    check(ukiyo::loadRuntimeScene(project + "/" + ukiyo::firstScene(manifest), project, &scene, &error),
          "the start scene loads", error);
    check(scene.warnings.empty(), "it loads without warnings",
          scene.warnings.empty() ? std::string() : scene.warnings.front());
    check(scene.instances.size() == 24, "twenty-four instances", std::to_string(scene.instances.size()));
    check(scene.lights.size() == 15, "fifteen lights, one under the sixteen a scene may author",
          std::to_string(scene.lights.size()));

    int stones = 0, papers = 0, scripts = 0, gates = 0, walls = 0;
    bool heroOk = false, groundOk = false, pathsExist = true;
    std::string missing, scriptPath;
    for (const ukiyo::RuntimeInstance& in : scene.instances) {
        if (in.name == "Hero") heroOk = in.components.character.enabled;
        // The ground is the scaled platform plus a separate invisible body (two instances on purpose: the mesh
        // is scaled, the collider says what it covers, and neither has to inherit the other's numbers).
        if (in.name == "Ground Body") groundOk = in.empty && in.components.body.enabled;
        if (in.name.rfind("Stone ", 0) == 0) ++stones;
        if (in.name.rfind("Lantern ", 0) == 0) ++papers;
        if (in.name.rfind("Gate ", 0) == 0) ++gates;
        if (in.name.rfind("Wall ", 0) == 0 && in.components.body.enabled) ++walls;
        if (in.components.script.enabled) {
            ++scripts;
            scriptPath = project + "/" + in.components.script.path;
        }
        if (!in.empty && !in.path.empty() && !fs::exists(in.path)) {
            pathsExist = false;
            if (missing.empty()) missing = in.name + " -> " + in.path;
        }
    }
    check(heroOk, "the hero is the character the player drives");
    check(groundOk, "the ground is a static body, not just a mesh");
    check(stones == 8, "eight stone lanterns", std::to_string(stones));
    check(papers == 6, "six paper lanterns", std::to_string(papers));
    check(gates == 2, "two torii gates", std::to_string(gates));
    check(walls == 4, "four walls keep the walk inside the village", std::to_string(walls));
    check(scripts == 1, "one script", std::to_string(scripts));
    check(pathsExist, "every model the scene names is on disk", missing);
    check(!scriptPath.empty() && fs::exists(scriptPath), "the script is where the scene says it is", scriptPath);

    // --- The lights -----------------------------------------------------------
    std::set<std::string> names, lit, dark;
    int directional = 0, unnamed = 0;
    for (const ukiyo::SceneLight& l : scene.lights) {
        if (l.name.empty()) ++unnamed;
        if (!l.name.empty()) names.insert(l.name);
        if (l.type == ukiyo::LightType::Directional) ++directional;
        (l.enabled ? lit : dark).insert(l.name);
    }
    check(unnamed == 0, "every light carries the name the script addresses it by", std::to_string(unnamed));
    check(names.size() == scene.lights.size(), "no two lights share a name");
    check(directional == 1, "one sun", std::to_string(directional));
    const std::set<std::string> litAtDusk = {"Dusk", "Stone A", "Stone C", "Stone E", "Stone G"};
    const std::set<std::string> toRelight = {"Stone B", "Stone D", "Stone F", "Stone H",
                                             "Flame 1", "Flame 2", "Flame 3", "Flame 4", "Flame 5", "Flame 6"};
    check(lit == litAtDusk, "five lights burn at dusk: the sun and four stones",
          lit.empty() ? std::string() : "first is " + *lit.begin());
    check(dark == toRelight, "ten are out: four stones and six flames — the game's ten relights",
          std::to_string(dark.size()) + " dark");
    check(scene.lights.size() <= 16, "the scene fits kMaxSceneLights");

    // --- The script and its contract with the scene ---------------------------
    const std::string script = readText(scriptPath);
    check(!script.empty(), "the script reads");
    check(has(script, "function onStart") && has(script, "function onUpdate"),
          "it has the two hooks a running game needs");
    check(has(script, "saveWrite(0)") && has(script, "saveRead(0)") && has(script, "saveSlotExists(0)"),
          "progress lives in save slot 0");
    check(has(script, "playMusic") && has(script, "stopMusic"), "the soundtrack is wired both ways");
    bool allNames = true;
    std::string absent;
    for (const std::string& n : toRelight) {
        if (!has(script, '"' + n + '"')) {
            allNames = false;
            if (absent.empty()) absent = n;
        }
    }
    check(allNames, "the script names all ten relights exactly as the scene does", absent);
    const char* media[] = {"assets/music/dusk_loop.wav", "assets/audio/spark.wav", "assets/audio/bell.wav"};
    bool mediaWired = true;
    for (const char* m : media) mediaWired = mediaWired && has(script, m);
    check(mediaWired, "the script asks for the three files the audio tool writes");

    // --- The audio the committed generator wrote ------------------------------
    struct Expect { const char* file; double seconds; };
    const Expect expects[] = {{"assets/music/dusk_loop.wav", 26.667}, {"assets/audio/spark.wav", 0.35},
                              {"assets/audio/bell.wav", 3.0}};
    for (const Expect& e : expects) {
        const std::string path = project + "/" + e.file;
        uint32_t rate = 0;
        uint16_t channels = 0;
        double seconds = 0.0;
        const bool ok = wavInfo(path, &rate, &channels, &seconds) && rate == 48000 && channels == 1 &&
                        std::fabs(seconds - e.seconds) < 0.12;
        char detail[160];
        std::snprintf(detail, sizeof detail, "%s: %u Hz, %u ch, %.3f s (want ~%.2f)", e.file, rate, channels,
                      seconds, e.seconds);
        check(ok, e.file, detail);
    }

    // --- What a player reads --------------------------------------------------
    ukiyo::LocaleCatalog en;
    ukiyo::loadLocale(project, "en", &en, nullptr);
    ukiyo::LocaleCatalog ja;
    const bool jaOk = ukiyo::loadLocale(project, "ja", &ja, &error);
    check(en.has("complete") && en.has("progress") && en.has("near"), "English has every key the HUD asks for");
    check(jaOk && ja.has("complete") && ja.has("progress") && ja.has("near"),
          "Japanese has every key the HUD asks for", error);
    check(jaOk && ja.tr("complete") != en.tr("complete"), "the Japanese line is translated, not copied");

    // --- The dusk itself ------------------------------------------------------
    check(scene.settings.sky && scene.settings.bloom && scene.settings.outlines,
          "the dusk profile keeps sky, bloom and outlines");
    check(scene.settings.fogDensity > 0.0f && scene.settings.stars > 0.0f,
          "the fog is on and the first stars are up",
          std::to_string(scene.settings.fogDensity) + " / " + std::to_string(scene.settings.stars));

    std::printf("afterglow_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
