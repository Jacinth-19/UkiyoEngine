// The Android player's first launch, tested where it can run.
//
// On a phone, libukiyo_player.so copies the APK's assets into the app's internal storage before anything else; if
// that loop mis-stages a file, the game dies before its first frame, on a device CI cannot see. The loop itself,
// ukiyo::staging::stage, now lives in player/asset_staging.h with the APK side reduced to a reader callback — so
// this test drives the exact lines a phone runs, against a folder standing in for the APK: the list from the
// package, the parent directories, the exact bytes, the re-stage over an existing install, and the two ways the
// launch has to fail loudly (a missing list, a missing asset).
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "player/asset_staging.h"

namespace {

namespace fs = std::filesystem;

int g_pass = 0, g_fail = 0;

void check(bool ok, const char* name, const std::string& detail = "") {
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : "  -- ", detail.c_str());
    ok ? ++g_pass : ++g_fail;
}

// A folder that pretends to be an APK's asset tree.
struct ApkFolder {
    fs::path root;

    bool read(const std::string& name, std::vector<uint8_t>* out) const {
        std::ifstream f(root / name, std::ios::binary);
        if (!f) return false;
        out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return true;
    }
};

void writeFile(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string readFile(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    fs::path tmp = fs::temp_directory_path() / "ukiyo_stage_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "apk");
    fs::create_directories(tmp / "storage");

    // The package: a list that looks like the real one -- both games, a font, a nested scene, a binary blob --
    // plus the files it names. The binary blob is not valid text on purpose: staging must be byte-honest.
    ApkFolder apk{tmp / "apk"};
    writeFile(apk.root / "games/afterglow/Build/build.json", "{\"format\":\"ukiyo-build\"}");
    writeFile(apk.root / "games/afterglow/Scripts/game.lua", "function onUpdate(dt) end\n");
    writeFile(apk.root / "games/lantern-run/Build/build.json", "{\"format\":\"ukiyo-build\"}");
    writeFile(apk.root / "editor/res/fonts/IBMPlexSansJP-Regular.ttf", std::string(4096, '\xBF'));
    writeFile(apk.root / "assets/AliciaSolid_vrm-0.51.vrm", std::string("GLB....\0\1\2\xFF", 10));
    writeFile(apk.root / "files.txt",
              "games/afterglow/Build/build.json\n"
              "games/afterglow/Scripts/game.lua\n"
              "games/lantern-run/Build/build.json\n"
              "editor/res/fonts/IBMPlexSansJP-Regular.ttf\n"
              "assets/AliciaSolid_vrm-0.51.vrm\n");

    // --- the launch ---------------------------------------------------------------------------
    std::string error;
    const std::string storage = (tmp / "storage").string();
    const int staged = ukiyo::staging::stage(&apk, [](ApkFolder* a, const std::string& name,
                                                      std::vector<uint8_t>* out) { return a->read(name, out); },
                                             storage, &error);
    check(staged == 5, "the whole package stages", error + " (" + std::to_string(staged) + ")");

    bool all = true;
    std::string detail;
    for (const char* name : {"games/afterglow/Build/build.json", "games/afterglow/Scripts/game.lua",
                             "games/lantern-run/Build/build.json", "editor/res/fonts/IBMPlexSansJP-Regular.ttf",
                             "assets/AliciaSolid_vrm-0.51.vrm"}) {
        const std::string want = readFile(apk.root / name);
        const std::string have = readFile(fs::path(storage) / name);
        if (want != have) {
            all = false;
            detail = name;
            break;
        }
    }
    check(all, "every staged file is byte-identical to the package", detail);
    check(readFile(fs::path(storage) / "assets/AliciaSolid_vrm-0.51.vrm").size() == 10,
          "a binary asset survives with its bytes, not as text");

    // --- the second launch --------------------------------------------------------------------
    writeFile(fs::path(storage) / "games/afterglow/Build/build.json", "stale bytes from an older install");
    const int restaged = ukiyo::staging::stage(&apk, [](ApkFolder* a, const std::string& name,
                                                        std::vector<uint8_t>* out) { return a->read(name, out); },
                                               storage, &error);
    check(restaged == 5, "re-staging over an existing install succeeds", error);
    check(readFile(fs::path(storage) / "games/afterglow/Build/build.json") == "{\"format\":\"ukiyo-build\"}",
          "re-staging replaces a stale file");

    // --- the failures -------------------------------------------------------------------------
    {
        ApkFolder broken{tmp / "apk"};
        std::vector<uint8_t> keep = {};
        (void)keep;
        fs::rename(apk.root / "files.txt", tmp / "files.txt.bak");
        const int missing = ukiyo::staging::stage(&broken, [](ApkFolder* a, const std::string& name,
                                                              std::vector<uint8_t>* out) { return a->read(name, out); },
                                                  (tmp / "storage2").string(), &error);
        check(missing == -1 && error.find("files.txt") != std::string::npos,
              "a package without its list fails with a message", error);
        fs::rename(tmp / "files.txt.bak", apk.root / "files.txt");

        fs::remove(apk.root / "assets/AliciaSolid_vrm-0.51.vrm");
        const int hole = ukiyo::staging::stage(&apk, [](ApkFolder* a, const std::string& name,
                                                        std::vector<uint8_t>* out) { return a->read(name, out); },
                                               (tmp / "storage3").string(), &error);
        check(hole == -1 && error.find("AliciaSolid") != std::string::npos,
              "a listed file missing from the package fails with its name", error);
    }

    std::printf("android_stage: %d checks, %d failure(s)\n", g_pass + g_fail, g_fail);
    return g_fail == 0 ? 0 : 1;
}
