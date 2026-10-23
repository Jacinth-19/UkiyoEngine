// The project's assets: sidecars, identity, references, moves and importer settings.
//
// The properties a game project needs are the ones a broken path takes away: a GUID that survives a rename, a scan that
// says which files have no sidecar, a check that names every reference pointing at nothing, and a repair that fixes
// them after the files moved — including moves made outside the editor.
#include "engine/asset_db.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace ukiyo;

namespace {
int checks = 0;
int failures = 0;
void check(bool ok, const char* what, const std::string& detail = std::string()) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s%s%s\n", what, detail.empty() ? "" : " -- ", detail.c_str());
    }
}

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

std::string read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s;
}
} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "ukiyo_asset_db_test";
    std::error_code ec;
    fs::remove_all(root, ec);

    write(root / "assets/hero.vrm", "not really a model");
    write(root / "assets/props/lantern.glb", "not really a model");
    write(root / "assets/tex/paper.png", "png");
    write(root / "Scenes/Court.ukscene", R"({
      "format": "ukiyo-scene",
      "models": [
        {"name": "Hero", "path": "assets/hero.vrm"},
        {"name": "Lantern", "path": "assets/props/lantern.glb"}
      ]
    })");
    write(root / "Build/build.json", R"({"format":"ukiyo-build","version":1,"name":"Court","startScene":"Scenes/Court.ukscene","scenes":["Scenes/Court.ukscene"]})");

    // --- A scan creates the sidecars and knows what each file is -------------------------------------
    {
        AssetDatabase db = scanProject(root.string(), true);
        check(db.warnings.empty(), "a clean project scans without warnings",
              db.warnings.empty() ? "" : db.warnings.front());
        check(db.assets.size() == 4, "four assets: two models, a texture and a scene",
              std::to_string(db.assets.size()));
        const AssetEntry* hero = db.find("assets/hero.vrm");
        check(hero != nullptr, "the model is in the index");
        if (hero) {
            check(hero->meta.kind == AssetKind::Model, "its kind comes from the extension");
            check(hero->meta.guid.size() == 32, "it has a 32 character GUID", hero->meta.guid);
            check(fs::exists(root / "assets/hero.vrm.meta"), "a sidecar was written next to it");
        }
        const AssetEntry* scene = db.find("Scenes/Court.ukscene");
        check(scene && scene->meta.kind == AssetKind::Scene, "the scene is an asset too");
        check(fs::exists(root / ".ukiyo/assets.json"), "the project has an index");
    }

    // --- Identity is stable across scans, and a moved file keeps its GUID ---------------------------
    {
        AssetDatabase a = scanProject(root.string(), true);
        const std::string guid = a.find("assets/hero.vrm") ? a.find("assets/hero.vrm")->meta.guid : "";
        AssetDatabase b = scanProject(root.string(), true);
        check(b.find("assets/hero.vrm") && b.find("assets/hero.vrm")->meta.guid == guid, "the same file keeps its GUID");
        check(!b.assets.empty() && !b.assets.front().newMeta, "a second scan writes nothing new");
        check(assetGuidFor("assets/hero.vrm") == assetGuidFor("Assets/Hero.VRM"), "GUIDs ignore case");
        check(assetGuidFor("assets/hero.vrm") != assetGuidFor("assets/other.vrm"), "different paths differ");
    }

    // --- Importer settings survive the round trip ---------------------------------------------------
    {
        AssetMeta meta;
        meta.guid = "0123456789abcdef0123456789abcdef";
        meta.kind = AssetKind::Model;
        meta.importScale = 0.01f;
        meta.importYawDeg = 180.0f;
        meta.importLods = {8.0f, 20.0f};
        std::string err;
        check(writeAssetMeta((root / "assets/hero.vrm.meta").string(), meta, &err), "the sidecar writes", err);
        AssetMeta back;
        check(readAssetMeta((root / "assets/hero.vrm.meta").string(), &back, &err), "it reads back", err);
        check(back.guid == meta.guid && back.importScale == 0.01f && back.importYawDeg == 180.0f,
              "GUIDs and importer settings round trip");
        check(back.importLods.size() == 2 && back.importLods[1] == 20.0f, "the LOD ladder round trips");

        AssetDatabase db = scanProject(root.string(), false);
        const AssetEntry* hero = db.find("assets/hero.vrm");
        check(hero && hero->meta.importScale == 0.01f, "the scan reads the settings, not the default");
        if (hero) {
            const json entry = sceneEntryForAsset(*hero, "Hero");
            check(entry["path"] == "assets/hero.vrm", "the scene entry names the asset");
            check(entry.contains("scale") && entry["scale"][0].get<float>() == 0.01f,
                  "the import scale becomes the instance scale");
            check(entry.contains("lod") && entry["lod"].size() == 2, "the LOD ladder becomes the instance's");
            check(entry["rotation"][1].get<float>() == 180.0f, "the facing correction becomes the rotation");
        }
        // Put the defaults back for the rest of the test.
        AssetMeta plain = meta;
        plain.importScale = 1.0f;
        plain.importYawDeg = 0.0f;
        plain.importLods.clear();
        writeAssetMeta((root / "assets/hero.vrm.meta").string(), plain, &err);
    }

    // --- References: a missing target is named, a present one is not --------------------------------
    {
        AssetDatabase db = scanProject(root.string(), false);
        check(brokenReferences(db).empty(), "a project whose references resolve has none");
        write(root / "Scenes/Court.ukscene", R"({
          "format": "ukiyo-scene",
          "models": [{"name": "Hero", "path": "assets/hero.vrm"}, {"name": "Gone", "path": "assets/gone.glb"}]
        })");
        db = scanProject(root.string(), false);
        const auto broken = brokenReferences(db);
        check(broken.size() == 1, "the missing model is reported", std::to_string(broken.size()));
        if (!broken.empty()) {
            check(broken[0].target == "assets/gone.glb", "the report names the missing file", broken[0].target);
            check(broken[0].file == "Scenes/Court.ukscene", "and the document that points at it", broken[0].file);
            check(broken[0].field == "/models/1/path", "and the field", broken[0].field);
        }
        // A build manifest naming a scene that is not there is a package that cannot boot.
        write(root / "Build/build.json",
              R"({"format":"ukiyo-build","version":1,"name":"Court","startScene":"Scenes/Missing.ukscene","scenes":["Scenes/Missing.ukscene"]})");
        db = scanProject(root.string(), false);
        const auto manifestBroken = brokenReferences(db);
        bool named = false;
        for (const BrokenRef& b : manifestBroken)
            if (b.file == "Build/build.json" && b.target == "Scenes/Missing.ukscene") named = true;
        check(named, "the build manifest's missing scene is reported");
        write(root / "Scenes/Court.ukscene", R"({
          "format": "ukiyo-scene",
          "models": [{"name": "Hero", "path": "assets/hero.vrm"}, {"name": "Lantern", "path": "assets/props/lantern.glb"}]
        })");
        write(root / "Build/build.json", R"({"format":"ukiyo-build","version":1,"name":"Court","startScene":"Scenes/Court.ukscene","scenes":["Scenes/Court.ukscene"]})");
    }

    // --- A move inside the editor: the file, its sidecar, and every reference ------------------------
    {
        AssetDatabase db = scanProject(root.string(), true);
        int rewritten = 0;
        std::string err;
        check(moveAsset(&db, "assets/props/lantern.glb", "assets/lanterns/paper_lantern.glb", &rewritten, &err),
              "an asset moves", err);
        check(fs::exists(root / "assets/lanterns/paper_lantern.glb"), "the file is at its new path");
        check(fs::exists(root / "assets/lanterns/paper_lantern.glb.meta"), "the sidecar moved with it");
        check(!fs::exists(root / "assets/props/lantern.glb"), "and is gone from the old one");
        check(rewritten == 1, "the scene's reference was rewritten", std::to_string(rewritten));
        check(read(root / "Scenes/Court.ukscene").find("assets/lanterns/paper_lantern.glb") != std::string::npos,
              "the scene names the new path");
        check(brokenReferences(scanProject(root.string(), false)).empty(), "nothing is broken after the move");

        // Refusals: outside the project, onto an existing file, from something that is not an asset.
        check(!moveAsset(&db, "../outside.glb", "assets/x.glb", &rewritten, &err), "a move outside the project is refused");
        check(!moveAsset(&db, "assets/hero.vrm", "assets/tex/paper.png", &rewritten, &err), "a move onto an existing file is refused");
        check(!moveAsset(&db, "assets/nothing.glb", "assets/n.glb", &rewritten, &err), "a move of a file that is not there is refused");
    }

    // --- A move made outside the editor is repaired from the GUIDs the index kept --------------------
    {
        AssetDatabase before = scanProject(root.string(), true);
        const std::string guid = before.find("assets/hero.vrm")->meta.guid;
        fs::create_directories(root / "assets/characters", ec);
        fs::rename(root / "assets/hero.vrm", root / "assets/characters/hero.vrm", ec);
        fs::rename(root / "assets/hero.vrm.meta", root / "assets/characters/hero.vrm.meta", ec);

        AssetDatabase after = scanProject(root.string(), true);
        check(after.movedFrom.count(guid) == 1, "the scan recognises the move by GUID");
        if (after.movedFrom.count(guid)) check(after.movedFrom[guid] == "assets/hero.vrm", "and remembers the old path");

        const auto broken = brokenReferences(after);
        bool heroBroken = false;
        for (const BrokenRef& b : broken)
            if (b.target == "assets/hero.vrm") heroBroken = true;
        check(heroBroken, "the scene is broken until the repair runs");

        std::vector<std::string> files;
        const int fixed = repairMovedAssets(after, &files, nullptr);
        check(fixed == 1, "the repair rewrites the reference", std::to_string(fixed));
        check(read(root / "Scenes/Court.ukscene").find("assets/characters/hero.vrm") != std::string::npos,
              "the scene points at the new home");
        check(brokenReferences(scanProject(root.string(), false)).empty(), "the project is clean again");
    }

    // --- Identity rules ----------------------------------------------------------------------------
    {
        check(assetKindOf("a.GLB") == AssetKind::Model, "extensions are case insensitive");
        check(assetKindOf("notes.txt") == AssetKind::Unknown, "an unknown file is not an asset");
        check(looksLikeAssetPath("assets/hero.vrm"), "a relative model path is a reference");
        check(!looksLikeAssetPath("https://example.com/a.png"), "a URL is not");
        check(!looksLikeAssetPath("/abs/a.png"), "an absolute path is not");
        check(!looksLikeAssetPath("Hero"), "a plain name is not");
        check(assetKindFromName("prefab") == AssetKind::Prefab, "kind names round trip");
    }

    std::printf("asset_db_test: %d checks, %d failures\n", checks, failures);
    fs::remove_all(root, ec);
    return failures ? 1 : 0;
}
