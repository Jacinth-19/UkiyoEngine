#include "pack/pack.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

#include <nlohmann/json.hpp>

#include "engine/build_manifest.h"
#include "engine/scene_runtime.h"

namespace ukiyo {
namespace pack {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// A path with '/' separators and no "./", wherever it came from. The manifest stores these, and the zip writer needs
// them, and on Windows fs::path's native separators are the other slash.
std::string slash(const fs::path& p) {
    std::string s = p.generic_string();
    while (s.rfind("./", 0) == 0) s.erase(0, 2);
    return s;
}

std::vector<uint8_t> readAll(const fs::path& p) {
    std::vector<uint8_t> bytes;
    std::ifstream f(p, std::ios::binary);
    if (!f) return bytes;
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    f.seekg(0, std::ios::beg);
    if (size > 0) {
        bytes.resize(static_cast<size_t>(size));
        f.read(reinterpret_cast<char*>(bytes.data()), size);
        if (!f) bytes.clear();
    }
    return bytes;
}

bool writeAll(const fs::path& p, const void* data, size_t bytes) {
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    return f.good();
}

// Fails when `path` is not inside `root`, which is what stops a project from shipping ../../../../etc/passwd — not a
// threat model, but a typo in a model path should be a build error rather than a package with a hole in it.
bool insideRoot(const fs::path& root, const fs::path& path) {
    const fs::path r = fs::weakly_canonical(root);
    const fs::path p = fs::weakly_canonical(path);
    auto rIt = r.begin();
    auto pIt = p.begin();
    for (; rIt != r.end(); ++rIt, ++pIt) {
        if (pIt == p.end() || *rIt != *pIt) return false;
    }
    return true;
}

void addTree(const fs::path& root, const fs::path& dir, std::set<std::string>* out, std::string* error) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            *error = "cannot walk " + slash(dir) + ": " + ec.message();
            return;
        }
        if (!it->is_regular_file()) continue;
        out->insert(slash(fs::relative(it->path(), root)));
    }
}

// Every relative path a scene names, resolved against the project root. The scene loader already resolves model paths;
// scripts, sounds and the font are the ones this has to catch, and the loader's own fields carry them.
void addSceneReferences(const RuntimeScene& scene, std::set<std::string>* absPaths) {
    for (const RuntimeInstance& in : scene.instances) {
        if (!in.path.empty()) absPaths->insert(in.path);
        if (in.components.script.enabled && !in.components.script.path.empty())
            absPaths->insert(in.components.script.path);
        if (in.components.audio.enabled && !in.components.audio.sound.empty())
            absPaths->insert(in.components.audio.sound);
    }
}

} // namespace

uint32_t crc32(const void* data, size_t bytes) {
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        built = true;
    }
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < bytes; ++i) c = table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

bool collect(const std::string& rootDir, const std::string& projectDir, std::vector<std::string>* files,
             std::string* error) {
    error->clear();
    const fs::path root = fs::absolute(rootDir);
    const fs::path project = fs::absolute(projectDir);
    if (!fs::exists(project)) {
        *error = "no such project directory: " + projectDir;
        return false;
    }
    if (!insideRoot(root, project)) {
        *error = "the project is outside " + slash(root) + ": " + projectDir;
        return false;
    }

    std::set<std::string> paths;
    addTree(root, project, &paths, error);
    if (!error->empty()) return false;

    BuildManifest manifest;
    std::string why;
    if (!loadBuildManifest(slash(project / "Build" / "build.json"), &manifest, &why)) {
        *error = "cannot read the build manifest: " + why;
        return false;
    }

    std::vector<fs::path> references;
    if (!manifest.font.empty()) references.push_back(project / manifest.font);
    for (const std::string& scenePath : manifest.scenes) {
        const fs::path sceneFile = project / scenePath;
        RuntimeScene scene;
        if (!loadRuntimeScene(slash(sceneFile), slash(project), &scene, &why)) {
            *error = "cannot read " + scenePath + ": " + why;
            return false;
        }
        std::set<std::string> sceneFiles;
        addSceneReferences(scene, &sceneFiles);
        for (const std::string& f : sceneFiles) {
            // Script paths and sounds are project-relative; model paths are already absolute (the loader resolved them).
            const fs::path candidate(f);
            references.push_back(candidate.is_absolute() ? candidate : project / candidate);
        }
    }
    // A script can name a soundtrack no component field mentions: ukiyo.playMusic("Sound/beep.wav") is a string
    // inside a .lua file. The scripts the scenes reference are read for double-quoted media paths and resolved
    // against the project root exactly as World::resolveMediaPath resolves them at run time, so a package ships the
    // music instead of failing on the player's machine. The paths are then checked by the same existence loop as
    // every other reference: a script naming a file that is not there fails the build, which is the correct
    // outcome, and the message says which file.
    const size_t fromScenes = references.size();
    for (size_t i = 0; i < fromScenes; ++i) {
        const fs::path& script = references[i];
        if (script.extension() != ".lua") continue;
        std::ifstream in(script, std::ios::binary);
        if (!in) continue;
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (size_t q = 0; q < text.size(); ++q) {
            if (text[q] != '"') continue;
            const size_t end = text.find_first_of("\"\n", q + 1);
            if (end == std::string::npos || text[end] == '\n') break;   // an unclosed quote ends at the line
            std::string token = text.substr(q + 1, end - q - 1);
            q = end;
            std::string lower = token;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            const size_t dot = lower.rfind('.');
            if (dot == std::string::npos) continue;
            const char* exts[] = {".wav", ".ogg", ".mp3", ".flac"};
            bool media = false;
            for (const char* e : exts)
                if (lower.compare(dot, lower.size() - dot, e) == 0) media = true;
            if (!media) continue;
            const fs::path candidate(token);
            references.push_back(candidate.is_absolute() ? candidate : project / candidate);
        }
    }

    // A model can reference files the scene never names: glTF images[...].uri points at a texture beside the
    // model (Kenney's fantasy-town kit shares one Textures/colormap.png across every piece), and a .gltf can put
    // its vertex data in an external buffer the same way. Those names live inside the model file — the .glb's JSON
    // chunk or the .gltf itself — where the scene and the script scan above cannot see them, so the package would
    // silently ship a white model. Read each model reference, collect its external uris, and let the same existence
    // loop check them: a missing texture fails the build with its path, which is the correct outcome, and data:
    // uris are embedded and skipped.
    const size_t fromModels = references.size();
    for (size_t i = 0; i < fromModels; ++i) {
        const fs::path model = references[i];
        std::string ext = model.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (ext != ".glb" && ext != ".gltf") continue;
        const std::vector<uint8_t> bytes = readAll(model);
        if (bytes.size() < 12) {
            *error = "model is not a readable glTF file: " + slash(model);
            return false;
        }
        std::string text;
        const bool binary = bytes.size() >= 20 && std::memcmp(bytes.data(), "glTF", 4) == 0;
        if (binary) {
            const uint32_t jsonBytes = uint32_t(bytes[12]) | uint32_t(bytes[13]) << 8 | uint32_t(bytes[14]) << 16 |
                                       uint32_t(bytes[15]) << 24;
            const uint32_t jsonType = uint32_t(bytes[16]) | uint32_t(bytes[17]) << 8 | uint32_t(bytes[18]) << 16 |
                                      uint32_t(bytes[19]) << 24;
            if (jsonType != 0x4E4F534Au || size_t(jsonBytes) + 20 > bytes.size()) {
                *error = "model's JSON chunk is missing or malformed: " + slash(model);
                return false;
            }
            text.assign(bytes.begin() + 20, bytes.begin() + 20 + jsonBytes);
        } else {
            text.assign(bytes.begin(), bytes.end());
        }
        json root = json::parse(text, nullptr, false);
        if (root.is_discarded() || !root.is_object()) {
            *error = "model's JSON is not parseable: " + slash(model);
            return false;
        }
        for (const char* key : {"images", "buffers"}) {
            const auto it = root.find(key);
            if (it == root.end() || !it->is_array()) continue;
            for (const json& entry : *it) {
                const json& uri = entry["uri"];
                if (!uri.is_string()) continue;
                const std::string u = uri.get<std::string>();
                if (u.compare(0, 5, "data:") == 0) continue;   // embedded payload, nothing to ship
                const fs::path ref = model.parent_path() / fs::path(u);
                references.push_back(ref);
            }
        }
    }

    if (references.empty()) {
        *error = "the manifest names no font and no scenes";
        return false;
    }
    for (const fs::path& ref : references) {
        std::error_code ec;
        // Root containment first: `../../../../etc/passwd` exists, and the reason to refuse it is not that it is absent.
        if (!insideRoot(root, ref)) {
            *error = "referenced file is outside the package root: " + slash(ref);
            return false;
        }
        if (!fs::exists(ref, ec)) {
            *error = "referenced file is missing: " + slash(ref);
            return false;
        }
        if (fs::is_directory(ref)) {
            std::string walkError;
            addTree(root, ref, &paths, &walkError);
            if (!walkError.empty()) {
                *error = walkError;
                return false;
            }
            continue;
        }
        paths.insert(slash(fs::relative(ref, root)));
    }

    files->assign(paths.begin(), paths.end());
    return true;
}

bool build(const std::string& rootDir, const std::string& projectDir, const std::string& playerBinary,
           const std::string& platform, const std::string& shadersDir, const std::string& outDir, Result* result) {
    Result r;
    const fs::path root = fs::absolute(rootDir);
    const fs::path project = fs::absolute(projectDir);
    const fs::path out = fs::absolute(outDir);
    const bool windows = platform == "windows";

    std::vector<std::string> files;
    if (!collect(rootDir, projectDir, &files, &r.error)) return false;

    BuildManifest manifest;
    if (!loadBuildManifest(slash(project / "Build" / "build.json"), &manifest, &r.error)) return false;

    std::error_code ec;
    fs::create_directories(out, ec);
    if (ec) {
        r.error = "cannot create " + slash(out) + ": " + ec.message();
        return false;
    }
    if (fs::exists(out / "package.json")) {
        r.error = slash(out) + " already contains a package";
        return false;
    }

    const std::string playerName = windows ? "ukiyo_player.exe" : "ukiyo_player";
    const std::vector<uint8_t> player = readAll(playerBinary);
    if (player.empty()) {
        r.error = "cannot read the player binary: " + playerBinary;
        return false;
    }
    if (!writeAll(out / playerName, player.data(), player.size())) {
        r.error = "cannot write " + slash(out / playerName);
        return false;
    }
    // The binary is the other thing that has to be executable. A copied file is 0644 whatever it was; without this the
    // launcher's `exec ./ukiyo_player` fails with "Permission denied" and the package looks broken for a reason that has
    // nothing to do with the game.
    fs::permissions(out / playerName,
                    fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                        fs::perms::others_exec,
                    fs::perm_options::replace, ec);

    r.projectPath = slash(fs::relative(project, root));
    std::vector<FileEntry> entries;
    entries.push_back({playerName, player.size(), crc32(player.data(), player.size())});

    // The compiled shaders, under their own folder. They are as much a part of the build as the binary: a player that
    // cannot find them is a window that opens and stays black.
    if (!shadersDir.empty()) {
        const fs::path shaders = fs::absolute(shadersDir);
        if (!fs::exists(shaders / "spv")) {
            r.error = "no compiled shaders in " + shadersDir;
            return false;
        }
        std::error_code shaderEc;
        for (fs::recursive_directory_iterator it(shaders, shaderEc), end; it != end; it.increment(shaderEc)) {
            if (shaderEc) {
                r.error = "cannot walk " + shadersDir + ": " + shaderEc.message();
                return false;
            }
            if (!it->is_regular_file()) continue;
            const std::string rel = "shaders/" + slash(fs::relative(it->path(), shaders));
            const std::vector<uint8_t> bytes = readAll(it->path());
            if (!writeAll(out / rel, bytes.data(), bytes.size())) {
                r.error = "cannot write " + rel;
                return false;
            }
            entries.push_back({rel, bytes.size(), crc32(bytes.data(), bytes.size())});
        }
    }

    for (const std::string& rel : files) {
        const std::vector<uint8_t> bytes = readAll(root / rel);
        if (!writeAll(out / rel, bytes.data(), bytes.size())) {
            r.error = "cannot write " + rel;
            return false;
        }
        entries.push_back({rel, bytes.size(), crc32(bytes.data(), bytes.size())});
    }

    // The launcher. It runs the player with the same project path the editor used, so nothing in the project has to
    // know it is inside a package.
    const std::string start = firstScene(manifest);
    const std::string shaderFlag = shadersDir.empty() ? std::string() : " --shaders shaders";
    const std::string sh = "#!/bin/sh\n"
                           "# " + manifest.name + " — generated by ukiyo_pack. Runs the game this folder contains.\n"
                           "cd \"$(dirname \"$0\")\" || exit 1\n"
                           // A zip does not carry the executable bit, and not every extractor restores it: make sure the
                           // binary can run before asking the shell to replace itself with it.
                           "[ -x ./" + playerName + " ] || chmod +x ./" + playerName + " 2>/dev/null || true\n"
                           "exec ./" +
                           playerName + " --project " + r.projectPath + " --build Build/build.json" + shaderFlag +
                           " \"$@\"\n";
    const std::string bat = "@echo off\r\n"
                            "rem Generated by ukiyo_pack. Runs the game this folder contains.\r\n"
                            "cd /d \"%~dp0\"\r\n" +
                            std::string(windows ? "ukiyo_player.exe" : "ukiyo_player") + " --project " + r.projectPath +
                            " --build Build/build.json" + shaderFlag + " %*\r\n";
    (void)start;   // package.json records it; the launcher takes it from the manifest itself
    if (!writeAll(out / "run.sh", sh.data(), sh.size())) {
        r.error = "cannot write the launcher";
        return false;
    }
    // 0755 outright rather than "add the execute bits": a package assembled on a filesystem or through a copy that
    // dropped the mode must still produce a launcher a user can run with ./run.sh.
    fs::permissions(out / "run.sh",
                    fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                        fs::perms::others_exec,
                    fs::perm_options::replace, ec);
    if (!writeAll(out / "run.bat", bat.data(), bat.size())) {
        r.error = "cannot write the launcher";
        return false;
    }
    entries.push_back({"run.sh", sh.size(), crc32(sh.data(), sh.size())});
    entries.push_back({"run.bat", bat.size(), crc32(bat.data(), bat.size())});

    r.ok = true;
    r.gameName = manifest.name;
    r.gameVersion = manifest.version;
    r.files = entries;
    for (const FileEntry& e : entries) r.bytes += e.bytes;

    json doc;
    doc["format"] = "ukiyo-package";
    doc["version"] = 1;
    doc["game"] = manifest.name;
    doc["gameVersion"] = manifest.version;
    doc["platform"] = platform;
    doc["player"] = playerName;
    doc["project"] = r.projectPath;
    doc["startScene"] = start;
    doc["bytes"] = r.bytes;
    doc["fileCount"] = entries.size();
    json list = json::array();
    for (const FileEntry& e : entries) list.push_back({{"path", e.path}, {"bytes", e.bytes}, {"crc32", e.crc}});
    doc["files"] = list;
    const std::string text = doc.dump(2) + "\n";
    if (!writeAll(out / "package.json", text.data(), text.size())) {
        r.error = "cannot write package.json";
        return false;
    }

    if (result) *result = r;
    return true;
}

bool verify(const std::string& dir, std::string* error) {
    error->clear();
    const fs::path root(dir);
    const std::vector<uint8_t> bytes = readAll(root / "package.json");
    if (bytes.empty()) {
        *error = "no package.json in " + dir;
        return false;
    }
    const json doc = json::parse(std::string(bytes.begin(), bytes.end()), nullptr, false);
    if (doc.is_discarded() || doc.value("format", std::string()) != "ukiyo-package") {
        *error = "package.json is not a ukiyo package";
        return false;
    }

    std::set<std::string> listed;
    uint64_t total = 0;
    for (const json& f : doc.value("files", json::array())) {
        const std::string rel = f.value("path", std::string());
        const uint64_t size = f.value("bytes", uint64_t(0));
        const uint32_t crc = f.value("crc32", uint32_t(0));
        const fs::path path = root / rel;
        if (!fs::exists(path)) {
            *error = "listed but missing: " + rel;
            return false;
        }
        const std::vector<uint8_t> fileBytes = readAll(path);
        if (fileBytes.size() != size) {
            *error = rel + " is " + std::to_string(fileBytes.size()) + " bytes, the package says " +
                     std::to_string(size);
            return false;
        }
        if (crc32(fileBytes.data(), fileBytes.size()) != crc) {
            *error = rel + " does not match its recorded CRC-32";
            return false;
        }
        listed.insert(rel);
        total += size;
    }
    if (doc.contains("bytes") && doc["bytes"].get<uint64_t>() != total) {
        *error = "the package's byte total does not match its file list";
        return false;
    }

    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            *error = "cannot walk " + dir + ": " + ec.message();
            return false;
        }
        if (!it->is_regular_file()) continue;
        const std::string rel = slash(fs::relative(it->path(), root));
        if (rel == "package.json") continue;
        if (!listed.count(rel)) {
            *error = "on disk but not in the manifest: " + rel;
            return false;
        }
    }
    return true;
}

bool writeZip(const std::string& dir, const std::string& zipPath, std::string* error) {
    error->clear();
    const fs::path root(dir);
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            *error = "cannot walk " + dir + ": " + ec.message();
            return false;
        }
        if (it->is_regular_file()) names.push_back(slash(fs::relative(it->path(), root)));
    }
    std::sort(names.begin(), names.end());
    if (names.empty()) {
        *error = "nothing to archive in " + dir;
        return false;
    }

    std::vector<uint8_t> out;
    auto put16 = [&](uint16_t v) {
        out.push_back(uint8_t(v & 0xFF));
        out.push_back(uint8_t(v >> 8));
    };
    auto put32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(uint8_t((v >> (i * 8)) & 0xFF));
    };

    struct Central {
        std::string name;
        uint32_t crc, size, offset;
    };
    std::vector<Central> central;

    for (const std::string& name : names) {
        const std::vector<uint8_t> bytes = readAll(root / name);
        Central c{name, crc32(bytes.data(), bytes.size()), static_cast<uint32_t>(bytes.size()),
                  static_cast<uint32_t>(out.size())};
        central.push_back(c);
        put32(0x04034B50u);              // local file header
        put16(20);                       // version needed: 2.0 (store, no data descriptors)
        put16(0);                        // flags
        put16(0);                        // method: store
        put16(0);                        // time
        put16(0x21);                     // date: 1980-01-01, so two builds of one package are identical
        put32(c.crc);
        put32(c.size);
        put32(c.size);
        put16(static_cast<uint16_t>(name.size()));
        put16(0);                        // extra length
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), bytes.begin(), bytes.end());
    }

    const uint32_t directoryOffset = static_cast<uint32_t>(out.size());
    for (const Central& c : central) {
        put32(0x02014B50u);              // central directory header
        put16(20);                       // version made by
        put16(20);                       // version needed
        put16(0);
        put16(0);
        put16(0);
        put16(0x21);
        put32(c.crc);
        put32(c.size);
        put32(c.size);
        put16(static_cast<uint16_t>(c.name.size()));
        put16(0);
        put16(0);
        put16(0);
        put16(0);
        put32(0);                        // external attributes
        put32(c.offset);
        out.insert(out.end(), c.name.begin(), c.name.end());
    }
    const uint32_t directoryBytes = static_cast<uint32_t>(out.size()) - directoryOffset;
    put32(0x06054B50u);                  // end of central directory
    put16(0);
    put16(0);
    put16(static_cast<uint16_t>(central.size()));
    put16(static_cast<uint16_t>(central.size()));
    put32(directoryBytes);
    put32(directoryOffset);
    put16(0);

    if (!writeAll(zipPath, out.data(), out.size())) {
        *error = "cannot write " + zipPath;
        return false;
    }
    return true;
}

} // namespace pack
} // namespace ukiyo
