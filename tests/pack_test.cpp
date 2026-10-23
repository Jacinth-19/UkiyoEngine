// The packer, on a project small enough to hold in a test: what a package contains, that it verifies, that a corrupted
// file is caught, and that the zip it writes is a zip. A packer is the one tool whose failure a player sees and a
// developer does not, so every one of those is a check rather than a hope.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "pack/pack.h"

namespace {

namespace fs = std::filesystem;

int g_pass = 0, g_fail = 0;

void check(bool ok, const char* name, const std::string& detail = "") {
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : "  -- ", detail.c_str());
    ok ? ++g_pass : ++g_fail;
}

void writeFile(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string readFile(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

uint32_t u32(const std::string& b, size_t at) {
    return uint32_t(uint8_t(b[at])) | uint32_t(uint8_t(b[at + 1])) << 8 | uint32_t(uint8_t(b[at + 2])) << 16 |
           uint32_t(uint8_t(b[at + 3])) << 24;
}

uint16_t u16(const std::string& b, size_t at) {
    return uint16_t(uint8_t(b[at]) | uint8_t(b[at + 1]) << 8);
}

// A game project laid out the way Lantern Run is: its own directory, and references that leave it.
fs::path makeProject(const fs::path& root) {
    writeFile(root / "shared" / "font.ttf", "a font, honestly");
    writeFile(root / "shared" / "prop.glb", std::string(256, 'g'));
    writeFile(root / "build" / "ukiyo_player", std::string(1024, 'p'));
    writeFile(root / "build" / "shaders" / "spv" / "hud.frag.spv", std::string(64, 's'));
    writeFile(root / "build" / "shaders" / "gl" / "hud.frag", "// a shader\n");
    writeFile(root / "games" / "tiny" / "Build" / "build.json",
              R"({"format":"ukiyo-build","version":1,"name":"Tiny Game","versionName":"1.2.3",
                  "startScene":"Scenes/Main.ukscene","font":"../../shared/font.ttf",
                  "window":{"width":640,"height":360,"fullscreen":false,"vsync":true},
                  "scenes":["Scenes/Main.ukscene"]})");
    writeFile(root / "games" / "tiny" / "Scenes" / "Main.ukscene",
              R"({"format":"ukiyo-scene","version":2,"models":[
                    {"name":"Prop","path":"../../shared/prop.glb"},
                    {"name":"Runner","empty":true,"components":{"script":{"path":"Scripts/run.lua"}}}]})");
    writeFile(root / "games" / "tiny" / "Sound" / "beep.wav", "RIFF....WAVE a tone\n");
    writeFile(root / "games" / "tiny" / "Scripts" / "run.lua",
              "function onUpdate(dt) ukiyo.playMusic(\"Sound/beep.wav\", true) end\n");
    writeFile(root / "games" / "tiny" / "notes.txt", "not referenced, still part of the project\n");
    return root / "games" / "tiny";
}

struct ZipEntry {
    std::string name;
    std::string bytes;
    uint32_t crc = 0;
};

// Enough of a zip reader to say "this is a zip and the bytes in it are the bytes on disk": the central directory, and
// each entry's stored payload via its local header.
std::vector<ZipEntry> readZip(const std::string& path, bool* ok) {
    std::vector<ZipEntry> entries;
    *ok = false;
    const std::string bytes = readFile(path);
    if (bytes.size() < 22) return entries;
    // The end-of-central-directory record is the last 22 bytes when there is no comment.
    const size_t eocd = bytes.size() - 22;
    if (u32(bytes, eocd) != 0x06054B50u) return entries;
    const uint16_t count = u16(bytes, eocd + 10);
    const uint32_t dirOffset = u32(bytes, eocd + 16);
    size_t at = dirOffset;
    for (uint16_t i = 0; i < count; ++i) {
        if (u32(bytes, at) != 0x02014B50u) return entries;
        ZipEntry e;
        e.crc = u32(bytes, at + 16);
        const uint32_t size = u32(bytes, at + 24);
        const uint16_t nameLen = u16(bytes, at + 28);
        const uint16_t extraLen = u16(bytes, at + 30);
        const uint16_t commentLen = u16(bytes, at + 32);
        const uint32_t localOffset = u32(bytes, at + 42);
        e.name = bytes.substr(at + 46, nameLen);
        // The local header repeats the name and extra field before the data.
        const uint16_t localNameLen = u16(bytes, localOffset + 26);
        const uint16_t localExtraLen = u16(bytes, localOffset + 28);
        const size_t data = localOffset + 30 + localNameLen + localExtraLen;
        if (data + size > bytes.size()) return entries;
        e.bytes = bytes.substr(data, size);
        entries.push_back(e);
        at += 46 + nameLen + extraLen + commentLen;
    }
    *ok = true;
    return entries;
}

} // namespace

int main() {
    const fs::path temp = fs::temp_directory_path() / "ukiyo_pack_test";
    std::error_code ec;
    fs::remove_all(temp, ec);
    fs::create_directories(temp);
    const fs::path root = temp / "root";
    const fs::path project = makeProject(root);
    // One file outside the root, to prove a reference that leaves it is refused.
    writeFile(temp / "outside.txt", "not yours to ship");

    // --- collect -------------------------------------------------------------------------
    std::vector<std::string> files;
    std::string error;
    check(ukiyo::pack::collect(root.string(), project.string(), &files, &error), "the project's files are collected",
          error);
    auto has = [&](const char* name) {
        for (const std::string& f : files)
            if (f == name) return true;
        return false;
    };
    check(has("games/tiny/Build/build.json") && has("games/tiny/Scenes/Main.ukscene") &&
              has("games/tiny/Scripts/run.lua") && has("games/tiny/notes.txt"),
          "everything in the project directory ships, referenced or not");
    check(has("shared/font.ttf"), "the font outside the project ships at the path the project names it by");
    check(has("shared/prop.glb"), "so does a model a scene points at");
    check(has("games/tiny/Sound/beep.wav"),
          "a soundtrack the script names by string ships with the package");

    // --- build ---------------------------------------------------------------------------
    ukiyo::pack::Result result;
    const fs::path out = temp / "dist" / "tiny-linux";
    const std::string shaders = (root / "build" / "shaders").string();
    check(ukiyo::pack::build(root.string(), project.string(), (root / "build" / "ukiyo_player").string(), "linux",
                             shaders, out.string(), &result),
          "the package is built", result.error);
    check(result.gameName == "Tiny Game" && result.gameVersion == "1.2.3", "the package knows what game it is",
          result.gameName + " " + result.gameVersion);
    check(result.projectPath == "games/tiny", "it remembers the project's path inside the package", result.projectPath);
    check(fs::exists(out / "ukiyo_player"), "the player binary is in the package");
    check(fs::exists(out / "run.sh") && fs::exists(out / "run.bat"), "both launchers are written");
    const std::string launcher = readFile(out / "run.sh");
    check(launcher.find("--project games/tiny") != std::string::npos,
          "the launcher runs the project where the package put it");
    check(fs::exists(out / "shaders" / "spv" / "hud.frag.spv") && fs::exists(out / "shaders" / "gl" / "hud.frag"),
          "the compiled shaders ship with the game");
    check(launcher.find("--shaders shaders") != std::string::npos,
          "and the launcher points the player at them");

    // --- verify --------------------------------------------------------------------------
    error.clear();
    check(ukiyo::pack::verify(out.string(), &error), "a fresh package verifies", error);
    check(result.files.size() >= 8, "the manifest lists every file", std::to_string(result.files.size()));

    // A package with one byte changed is not the package the manifest describes.
    {
        const fs::path victim = out / "games" / "tiny" / "Scripts" / "run.lua";
        const std::string original = readFile(victim);
        writeFile(victim, original + "-- a byte\n");
        error.clear();
        check(!ukiyo::pack::verify(out.string(), &error), "a changed file fails verification", error);
        writeFile(victim, original);
    }
    {
        const fs::path extra = out / "stowaway.txt";
        writeFile(extra, "nobody asked for me\n");
        error.clear();
        check(!ukiyo::pack::verify(out.string(), &error), "an unlisted file fails verification", error);
        fs::remove(extra);
    }
    error.clear();
    check(ukiyo::pack::verify(out.string(), &error), "and the package verifies again once it is whole", error);

    // --- windows naming --------------------------------------------------------------------
    ukiyo::pack::Result winResult;
    const fs::path winOut = temp / "dist" / "tiny-windows";
    check(ukiyo::pack::build(root.string(), project.string(), (root / "build" / "ukiyo_player").string(), "windows",
                             shaders, winOut.string(), &winResult),
          "a windows package is built", winResult.error);
    check(fs::exists(winOut / "ukiyo_player.exe"), "the windows player keeps its extension");
    check(readFile(winOut / "run.bat").find("ukiyo_player.exe") != std::string::npos,
          "and the batch file runs it");

    // A build that shipped a player but no shaders would open a black window on the player's machine; the packer
    // refuses to write one.
    {
        ukiyo::pack::Result bad;
        error.clear();
        check(!ukiyo::pack::build(root.string(), project.string(), (root / "build" / "ukiyo_player").string(), "linux",
                                  (root / "build" / "no-such-shaders").string(), (temp / "dist" / "bad").string(), &bad),
              "a package without its shaders is refused", bad.error);
    }

    // --- zip -------------------------------------------------------------------------------
    const fs::path zipPath = temp / "dist" / "tiny-linux.zip";
    error.clear();
    check(ukiyo::pack::writeZip(out.string(), zipPath.string(), &error), "the package is archived", error);
    bool zipOk = false;
    const std::vector<ZipEntry> entries = readZip(zipPath.string(), &zipOk);
    check(zipOk, "the archive is a zip a reader can open");
    check(entries.size() == result.files.size() + 1, "every file and the manifest are in the archive",
          std::to_string(entries.size()));
    bool bytesMatch = true, crcsMatch = true;
    size_t compared = 0;
    for (const ZipEntry& e : entries) {
        const std::string onDisk = readFile(out / e.name);
        ++compared;
        if (onDisk != e.bytes) bytesMatch = false;
        if (ukiyo::pack::crc32(e.bytes.data(), e.bytes.size()) != e.crc) crcsMatch = false;
    }
    check(compared > 0 && bytesMatch, "every archived file is byte-identical to the package", std::to_string(compared));
    check(crcsMatch, "every CRC-32 in the archive matches its bytes");

    // --- refusals ---------------------------------------------------------------------------
    writeFile(root / "games" / "broken" / "Build" / "build.json",
              R"({"format":"ukiyo-build","version":1,"name":"Broken","startScene":"Scenes/Missing.ukscene",
                  "scenes":["Scenes/Missing.ukscene"]})");
    error.clear();
    check(!ukiyo::pack::collect(root.string(), (root / "games" / "broken").string(), &files, &error),
          "a scene that does not exist is refused", error);

    writeFile(root / "games" / "escaping" / "Build" / "build.json",
              R"({"format":"ukiyo-build","version":1,"name":"Escaping","startScene":"Scenes/Main.ukscene",
                  "scenes":["Scenes/Main.ukscene"]})");
    writeFile(root / "games" / "escaping" / "Scenes" / "Main.ukscene",
              R"({"format":"ukiyo-scene","version":2,"models":[{"name":"Out","path":"../../../outside.txt"}]})");
    error.clear();
    check(!ukiyo::pack::collect(root.string(), (root / "games" / "escaping").string(), &files, &error),
          "a reference outside the package root is refused", error);

    fs::remove_all(temp, ec);
    std::printf("pack_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
