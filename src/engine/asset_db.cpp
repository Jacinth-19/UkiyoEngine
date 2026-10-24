#include "engine/asset_db.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace ukiyo {

namespace {

const char* kIndexDir = ".ukiyo";
const char* kIndexFile = ".ukiyo/assets.json";

std::string slash(const fs::path& p) {
    std::string s = p.generic_string();
    return s;
}


bool isMetaPath(const std::string& p) { return p.size() > 5 && p.compare(p.size() - 5, 5, ".meta") == 0; }

// Directories that are never assets: version control, build output, the engine's own index, and the editor's
// caches.
bool skipDir(const std::string& name) {
    return name == ".git" || name == ".ukiyo" || name == "node_modules" || name == "dist" || name == "build" ||
           name == ".cache" || name == "__pycache__" || name == ".idea" || name == ".vscode";
}

// The extensions the browser shows and the importer understands. Anything else is still a file in the project, just not
// an asset with identity.
bool isAssetFile(const std::string& p) { return assetKindOf(p) != AssetKind::Unknown; }

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool writeFile(const fs::path& p, const std::string& bytes) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

// GUIDs are their own log of where an asset used to be: the index maps GUID -> path from the last scan.
std::map<std::string, std::string> loadIndex(const fs::path& root) {
    std::map<std::string, std::string> out;
    const std::string text = readFile(root / kIndexFile);
    if (text.empty()) return out;
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return out;
    for (auto it = j.begin(); it != j.end(); ++it)
        if (it.value().is_string()) out[it.key()] = it.value().get<std::string>();
    return out;
}

void saveIndex(const fs::path& root, const AssetDatabase& db) {
    json j = json::object();
    for (const AssetEntry& a : db.assets)
        if (!a.meta.guid.empty()) j[a.meta.guid] = a.path;
    std::error_code ec;
    fs::create_directories(root / kIndexDir, ec);
    writeFile(root / kIndexFile, j.dump(2) + "\n");
}

// Every reference in a document is a string, so the rewrite is a walk over strings. It only replaces a string that is
// exactly the old path, which is what a scene, prefab or material stores.
int rewriteStrings(json* doc, const std::map<std::string, std::string>& remap) {
    int changed = 0;
    if (doc->is_string()) {
        auto it = remap.find(doc->get<std::string>());
        if (it != remap.end() && it->second != it->first) {
            *doc = it->second;
            ++changed;
        }
        return changed;
    }
    if (doc->is_array()) {
        for (json& v : *doc) changed += rewriteStrings(&v, remap);
        return changed;
    }
    if (doc->is_object()) {
        for (auto it = doc->begin(); it != doc->end(); ++it) changed += rewriteStrings(&it.value(), remap);
    }
    return changed;
}

// Deliberately not shared with json_patch: this one is used on documents that may be malformed, and an escape that
// throws would take the whole scan down.
std::string jsonPointerEscapeLocal(const std::string& key) {
    std::string out;
    for (char c : key) {
        if (c == '~') out += "~0";
        else if (c == '/') out += "~1";
        else out += c;
    }
    return out;
}

void collectStrings(const json& doc, const std::string& path, const std::string& file,
                    std::vector<std::pair<std::string, std::string>>* out) {
    if (doc.is_string()) {
        out->emplace_back(path, doc.get<std::string>());
        return;
    }
    if (doc.is_array()) {
        for (size_t i = 0; i < doc.size(); ++i)
            collectStrings(doc[i], path + "/" + std::to_string(i), file, out);
        return;
    }
    if (doc.is_object()) {
        for (auto it = doc.begin(); it != doc.end(); ++it)
            collectStrings(it.value(), path + "/" + jsonPointerEscapeLocal(it.key()), file, out);
    }
}

bool insideProject(const std::string& rel) {
    if (rel.empty() || rel[0] == '/' || rel[0] == '\\') return false;
    if (rel.size() > 1 && rel[1] == ':') return false;   // a Windows drive letter
    int depth = 0;
    size_t start = 0;
    while (start <= rel.size()) {
        const size_t slashAt = rel.find_first_of("/\\", start);
        const std::string part = rel.substr(start, slashAt == std::string::npos ? std::string::npos : slashAt - start);
        if (part == "..") {
            if (--depth < 0) return false;
        } else if (!part.empty() && part != ".") {
            ++depth;
        }
        if (slashAt == std::string::npos) break;
        start = slashAt + 1;
    }
    return true;
}

} // namespace

const char* assetKindName(AssetKind kind) {
    switch (kind) {
        case AssetKind::Model: return "model";
        case AssetKind::Texture: return "texture";
        case AssetKind::Scene: return "scene";
        case AssetKind::Prefab: return "prefab";
        case AssetKind::Material: return "material";
        case AssetKind::Shader: return "shader";
        case AssetKind::Script: return "script";
        case AssetKind::Audio: return "audio";
        case AssetKind::Terrain: return "terrain";
        case AssetKind::Font: return "font";
        case AssetKind::Unknown: break;
    }
    return "unknown";
}

AssetKind assetKindFromName(const std::string& name) {
    if (name == "model") return AssetKind::Model;
    if (name == "texture") return AssetKind::Texture;
    if (name == "scene") return AssetKind::Scene;
    if (name == "prefab") return AssetKind::Prefab;
    if (name == "material") return AssetKind::Material;
    if (name == "shader") return AssetKind::Shader;
    if (name == "script") return AssetKind::Script;
    if (name == "audio") return AssetKind::Audio;
    if (name == "terrain") return AssetKind::Terrain;
    if (name == "font") return AssetKind::Font;
    return AssetKind::Unknown;
}

AssetKind assetKindOf(const std::string& path) {
    std::string ext;
    const size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) {
        ext = path.substr(dot + 1);
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (ext == "vrm" || ext == "glb" || ext == "gltf" || ext == "pmx" || ext == "obj" || ext == "fbx")
        return AssetKind::Model;
    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp" || ext == "tga" || ext == "webp" ||
        ext == "gif" || ext == "tif" || ext == "tiff" || ext == "hdr" || ext == "exr")
        return AssetKind::Texture;
    if (ext == "ukscene") return AssetKind::Scene;
    if (ext == "ukprefab") return AssetKind::Prefab;
    if (ext == "ukmat") return AssetKind::Material;
    if (ext == "ukshader") return AssetKind::Shader;
    if (ext == "lua") return AssetKind::Script;
    if (ext == "wav" || ext == "mp3" || ext == "ogg" || ext == "flac" || ext == "opus" || ext == "m4a")
        return AssetKind::Audio;
    if (ext == "hgt" || ext == "heightmap") return AssetKind::Terrain;
    if (ext == "ttf" || ext == "otf") return AssetKind::Font;
    return AssetKind::Unknown;
}

bool looksLikeAssetPath(const std::string& value) {
    if (value.size() < 4 || value.size() > 512) return false;
    if (value.find("://") != std::string::npos) return false;
    if (value[0] == '/' || value[0] == '\\' || value[0] == '#') return false;
    if (value.find_first_of("\n\r\t") != std::string::npos) return false;
    return assetKindOf(value) != AssetKind::Unknown;
}

std::string assetGuidFor(const std::string& relativePath) {
    // FNV-1a over the path, twice with different offsets, printed as the 32 hex characters a GUID is usually written as.
    // It is not cryptographic and does not need to be: it distinguishes assets in one project.
    auto fnv = [](const std::string& s, uint32_t basis) {
        uint32_t h = basis;
        for (unsigned char c : s) {
            h ^= c;
            h *= 16777619u;
        }
        return h;
    };
    std::string p = relativePath;
    for (char& c : p) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%08x%08x%08x%08x", fnv(p, 2166136261u), fnv(p, 3735928559u),
                  fnv(p, 1013904223u), fnv(p, 2654435761u));
    return buf;
}

std::string assetMetaJson(const AssetMeta& meta) {
    json j = json::object();
    j["format"] = "ukiyo-asset";
    j["version"] = 1;
    j["guid"] = meta.guid;
    j["kind"] = assetKindName(meta.kind);
    json importer = json::object();
    importer["scale"] = meta.importScale;
    importer["yawDeg"] = meta.importYawDeg;
    importer["lods"] = meta.importLods;
    if (!meta.materialRemap.empty()) importer["materialRemap"] = meta.materialRemap;
    j["importer"] = importer;
    return j.dump(2) + "\n";
}

bool readAssetMeta(const std::string& path, AssetMeta* out, std::string* error) {
    const std::string text = readFile(path);
    if (text.empty()) {
        if (error) *error = "missing " + path;
        return false;
    }
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        if (error) *error = path + " is not JSON";
        return false;
    }
    const auto format = j.find("format");
    if (format == j.end() || !format->is_string() || format->get<std::string>() != "ukiyo-asset") {
        if (error) *error = path + " is not an asset sidecar";
        return false;
    }
    if (!out) return true;
    out->guid = j.value("guid", std::string());
    out->kind = assetKindFromName(j.value("kind", std::string("unknown")));
    const json importer = j.value("importer", json::object());
    out->importScale = importer.value("scale", 1.0f);
    out->importYawDeg = importer.value("yawDeg", 0.0f);
    out->materialRemap = importer.value("materialRemap", std::string());
    out->importLods.clear();
    if (importer.contains("lods") && importer["lods"].is_array())
        for (const json& v : importer["lods"])
            if (v.is_number()) out->importLods.push_back(v.get<float>());
    return true;
}

bool writeAssetMeta(const std::string& path, const AssetMeta& meta, std::string* error) {
    if (meta.guid.empty()) {
        if (error) *error = "an asset sidecar needs a GUID";
        return false;
    }
    if (!writeFile(path, assetMetaJson(meta))) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    return true;
}

const AssetEntry* AssetDatabase::find(const std::string& path) const {
    for (const AssetEntry& a : assets)
        if (a.path == path) return &a;
    return nullptr;
}

AssetDatabase scanProject(const std::string& root, bool createMissingMeta) {
    AssetDatabase db;
    db.root = root;
    const fs::path base(root);
    std::error_code ec;
    if (!fs::is_directory(base, ec)) {
        db.warnings.push_back("not a project directory: " + root);
        return db;
    }

    const std::map<std::string, std::string> previous = loadIndex(base);
    std::map<std::string, int> seenGuids;

    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        const fs::path& p = it->path();
        if (it->is_directory(ec)) {
            if (skipDir(p.filename().string())) it.disable_recursion_pending();
            continue;
        }
        const std::string rel = slash(fs::relative(p, base, ec));
        if (rel.empty() || isMetaPath(rel)) continue;
        if (isAssetFile(rel)) files.push_back(p);
    }
    std::sort(files.begin(), files.end());

    for (const fs::path& p : files) {
        const std::string rel = slash(fs::relative(p, base, ec));
        AssetEntry entry;
        entry.path = rel;
        entry.metaPath = rel + ".meta";
        entry.meta.kind = assetKindOf(rel);
        std::string err;
        if (fs::exists(base / entry.metaPath, ec)) {
            if (!readAssetMeta((base / entry.metaPath).string(), &entry.meta, &err)) {
                db.warnings.push_back(err);
                entry.meta = AssetMeta{};
                entry.meta.kind = assetKindOf(rel);
            }
        }
        if (entry.meta.guid.empty()) {
            entry.meta.guid = assetGuidFor(rel);
            if (entry.meta.kind == AssetKind::Unknown) entry.meta.kind = assetKindOf(rel);
            if (createMissingMeta && writeAssetMeta((base / entry.metaPath).string(), entry.meta, &err)) {
                entry.newMeta = true;
            } else if (createMissingMeta) {
                db.warnings.push_back(err);
            }
        }
        if (previous.count(entry.meta.guid)) {
            const std::string& was = previous.at(entry.meta.guid);
            if (was != rel) db.movedFrom[entry.meta.guid] = was;
        }
        if (++seenGuids[entry.meta.guid] > 1)
            db.warnings.push_back("two assets share the GUID " + entry.meta.guid + " (one is " + rel + ")");
        db.assets.push_back(std::move(entry));
    }

    if (createMissingMeta) saveIndex(base, db);
    return db;
}

std::vector<BrokenRef> brokenReferences(const AssetDatabase& db) {
    std::vector<BrokenRef> broken;
    const fs::path base(db.root);
    std::error_code ec;
    std::set<std::string> known;
    for (const AssetEntry& a : db.assets) known.insert(a.path);

    for (const AssetEntry& a : db.assets) {
        // Only documents that hold references. A texture has no JSON to read.
        if (a.meta.kind != AssetKind::Scene && a.meta.kind != AssetKind::Prefab && a.meta.kind != AssetKind::Material &&
            a.meta.kind != AssetKind::Shader)
            continue;
        const std::string text = readFile(base / a.path);
        if (text.empty()) continue;
        const json doc = json::parse(text, nullptr, false);
        if (doc.is_discarded()) continue;
        std::vector<std::pair<std::string, std::string>> strings;
        collectStrings(doc, "", a.path, &strings);
        const std::string dir = fs::path(a.path).parent_path().generic_string();
        for (const auto& [pointer, value] : strings) {
            if (!looksLikeAssetPath(value)) continue;
            if (known.count(value)) continue;
            // A scene may name a path relative to itself or to the project; both are what the editors write.
            if (fs::exists(base / value, ec)) continue;
            if (!dir.empty() && fs::exists(base / dir / value, ec)) continue;
            broken.push_back({a.path, pointer, value});
        }
    }
    // The build manifest names scenes, and a missing start scene is the one that makes a package fail to boot.
    const fs::path manifest = base / "Build" / "build.json";
    if (fs::exists(manifest, ec)) {
        const json doc = json::parse(readFile(manifest), nullptr, false);
        if (!doc.is_discarded() && doc.is_object()) {
            std::vector<std::pair<std::string, std::string>> strings;
            collectStrings(doc, "", "Build/build.json", &strings);
            for (const auto& [pointer, value] : strings) {
                if (!looksLikeAssetPath(value)) continue;
                if (known.count(value) || fs::exists(base / value, ec)) continue;
                broken.push_back({"Build/build.json", pointer, value});
            }
        }
    }
    return broken;
}

int repairMovedAssets(const AssetDatabase& db, std::vector<std::string>* files,
                      std::vector<std::string>* warnings) {
    if (db.movedFrom.empty()) return 0;
    std::map<std::string, std::string> remap;
    for (const auto& [guid, was] : db.movedFrom) {
        const AssetEntry* now = nullptr;
        for (const AssetEntry& a : db.assets)
            if (a.meta.guid == guid) now = &a;
        if (now && now->path != was) remap[was] = now->path;
    }
    if (remap.empty()) return 0;
    const fs::path base(db.root);
    std::error_code ec;
    int total = 0;
    for (fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (it->is_directory(ec)) {
            if (skipDir(it->path().filename().string())) it.disable_recursion_pending();
            continue;
        }
        const std::string rel = slash(fs::relative(it->path(), base, ec));
        const AssetKind kind = assetKindOf(rel);
        const bool isManifest = rel == "Build/build.json";
        if (kind != AssetKind::Scene && kind != AssetKind::Prefab && kind != AssetKind::Material &&
            kind != AssetKind::Shader && !isManifest)
            continue;
        const std::string text = readFile(it->path());
        json doc = json::parse(text, nullptr, false);
        if (doc.is_discarded()) continue;
        const int changed = rewriteStrings(&doc, remap);
        if (changed == 0) continue;
        if (!writeFile(it->path(), doc.dump(2) + "\n")) {
            if (warnings) warnings->push_back("could not rewrite " + rel);
            continue;
        }
        total += changed;
        if (files) files->push_back(rel);
    }
    return total;
}

bool moveAsset(AssetDatabase* db, const std::string& oldRelative, const std::string& newRelative, int* rewritten,
               std::string* error) {
    if (rewritten) *rewritten = 0;
    if (!db) return false;
    if (!insideProject(oldRelative) || !insideProject(newRelative)) {
        if (error) *error = "a move has to stay inside the project";
        return false;
    }
    const fs::path base(db->root);
    std::error_code ec;
    if (!fs::exists(base / oldRelative, ec)) {
        if (error) *error = "no such asset: " + oldRelative;
        return false;
    }
    if (fs::exists(base / newRelative, ec)) {
        if (error) *error = "already exists: " + newRelative;
        return false;
    }
    const AssetEntry* entry = db->find(oldRelative);
    if (!entry) {
        if (error) *error = oldRelative + " is not an asset this project knows";
        return false;
    }
    const std::string oldMeta = oldRelative + ".meta";
    const std::string newMeta = newRelative + ".meta";

    fs::create_directories((base / newRelative).parent_path(), ec);
    fs::rename(base / oldRelative, base / newRelative, ec);
    if (ec) {
        if (error) *error = "cannot move " + oldRelative + ": " + ec.message();
        return false;
    }
    if (fs::exists(base / oldMeta, ec)) fs::rename(base / oldMeta, base / newMeta, ec);

    // The reference rewrite is the same code that repairs a move made outside the editor.
    AssetDatabase after = scanProject(db->root, true);
    after.movedFrom[entry->meta.guid] = oldRelative;
    std::vector<std::string> files;
    const int count = repairMovedAssets(after, &files, nullptr);

    // And the scene the editor has in memory: whoever called this reloads, but the database it holds should describe
    // the project as it is now.
    db->assets = after.assets;
    db->movedFrom = after.movedFrom;
    if (rewritten) *rewritten = count;
    if (error) error->clear();
    return true;
}

json sceneEntryForAsset(const AssetEntry& entry, const std::string& name) {
    json m = json::object();
    m["name"] = name.empty() ? fs::path(entry.path).stem().string() : name;
    m["path"] = entry.path;
    const float scale = entry.meta.importScale;
    if (scale != 1.0f) m["scale"] = json::array({scale, scale, scale});
    if (entry.meta.importYawDeg != 0.0f) m["rotation"] = json::array({0.0f, entry.meta.importYawDeg, 0.0f});
    if (!entry.meta.importLods.empty()) m["lod"] = entry.meta.importLods;
    return m;
}

} // namespace ukiyo
