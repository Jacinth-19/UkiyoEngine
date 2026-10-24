// The project's assets: identity, import settings, references and moves.
//
// Until now an asset was a string in a scene file. Renaming a model, moving a folder, or opening a project that someone
// rearranged left scenes pointing at paths that no longer existed — and nothing noticed, because a scene loads its
// instances lazily and the player only logs what it cannot open.
//
// Every asset now has a sidecar: `<file>.meta`, holding a GUID that belongs to the asset rather than to its path, and
// the importer settings the editor applies when the asset enters a scene. The project also keeps an index of
// guid -> path (`.ukiyo/assets.json`). Together they make three things possible that were not:
//
//   * scan: which files are assets, which have no sidecar, which sidecars have no file;
//   * check: which scenes, prefabs, materials and build manifests point at files that are not there;
//   * repair: after files moved — in this editor or in Explorer — rewrite the references by GUID.
//
// Nothing here touches Qt, the GPU or the scene format the player reads. The reference it rewrites is still a plain
// relative path, so a shipped project needs no runtime support for any of this.
#pragma once

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace ukiyo {

using json = nlohmann::json;

enum class AssetKind { Unknown, Model, Texture, Scene, Prefab, Material, Shader, Script, Audio, Terrain, Font };

const char* assetKindName(AssetKind kind);
AssetKind assetKindFromName(const std::string& name);
// The kind a file extension implies, or Unknown.
AssetKind assetKindOf(const std::string& path);

struct AssetMeta {
    std::string guid;          // belongs to the asset, survives a rename
    AssetKind kind = AssetKind::Unknown;
    // Importer settings, applied by the editor when the asset is added to a scene. Defaults are "no change".
    float importScale = 1.0f;  // models: multiplies the instance scale written into the scene
    float importYawDeg = 0.0f; // models: a facing correction, for art authored looking down -Z
    std::vector<float> importLods;   // models: LOD switch distances, empty for none
    std::string materialRemap;       // models: project-relative .ukmat applied to every material, empty for none
};

// `<asset>.meta` next to the asset. False when the file is missing or is not a meta document.
bool readAssetMeta(const std::string& path, AssetMeta* out, std::string* error);
std::string assetMetaJson(const AssetMeta& meta);
bool writeAssetMeta(const std::string& path, const AssetMeta& meta, std::string* error);

// A stable GUID for a path. Deterministic so two machines that scan the same project agree, and salted with the asset's
// own path so a fresh sidecar on a copy of the project still names the copy's asset.
std::string assetGuidFor(const std::string& relativePath);

struct AssetEntry {
    std::string path;      // project-relative, forward slashes
    std::string metaPath;  // the sidecar's path, empty when it has none
    AssetMeta meta;
    bool newMeta = false;  // the scan created it
};

struct BrokenRef {
    std::string file;    // the document holding the reference
    std::string field;   // its JSON pointer, for an editor to select
    std::string target;  // the path that is not there
};

struct AssetDatabase {
    std::string root;
    std::vector<AssetEntry> assets;
    std::map<std::string, std::string> movedFrom;   // guid -> the path the last scan saw it at
    std::vector<std::string> warnings;              // duplicate GUIDs, unreadable sidecars
    const AssetEntry* find(const std::string& path) const;
};

// Walks the project, skipping VCS and build output. `createMissingMeta` writes a sidecar for every asset that has none,
// which is what the editor does on open; the test uses false to look without touching anything.
AssetDatabase scanProject(const std::string& root, bool createMissingMeta);

// References from scenes, prefabs, materials, shaders and the build manifest that do not resolve. JSON strings that
// look like asset paths are checked against `db`; a path that exists on disk is never reported, so a project the
// database has not scanned yet does not produce noise.
std::vector<BrokenRef> brokenReferences(const AssetDatabase& db);

// Rewrites references from their old path to a moved asset's new one, using the GUIDs the last scan recorded. Returns
// the number of references rewritten; `files` receives the documents that changed.
int repairMovedAssets(const AssetDatabase& db, std::vector<std::string>* files, std::vector<std::string>* warnings);

// Moves an asset and its sidecar, rewriting every reference to it. `newRelative` must stay inside the project.
bool moveAsset(AssetDatabase* db, const std::string& oldRelative, const std::string& newRelative, int* rewritten,
               std::string* error);

// The scene entry an imported asset becomes: path, scale, rotation and LOD ladder from its importer settings. This is
// the whole point of the settings existing — the editor inserts exactly this object.
json sceneEntryForAsset(const AssetEntry& entry, const std::string& name);

// True when `value` looks like a project-relative asset path (an extension this engine knows, no URL, not absolute).
bool looksLikeAssetPath(const std::string& value);

} // namespace ukiyo
