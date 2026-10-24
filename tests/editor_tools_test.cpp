// The pieces of the editor that are not the editor: packaging arguments, git status, localisation catalogues.
//
// Each of these is a small translation of something external — the packer's flags, git's porcelain output, a
// translator's JSON — and each is exactly the kind of code that is wrong in a way nobody notices until a shipped game
// has a path missing or a menu says a repository has no changes when it has twenty.
#include "engine/localisation.h"
#include "engine/vcs.h"
#include "pack/pack_cli.h"

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
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
bool hasArgPair(const std::vector<std::string>& args, const std::string& name, const std::string& value) {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == name && args[i + 1] == value) return true;
    return false;
}
} // namespace

int main() {
    // --- Packaging: the flags the packer takes ------------------------------------------------------
    {
        pack::PackRequest r;
        r.project = "games/lantern-run";
        r.player = "build/ukiyo_player";
        r.shaders = "build/shaders";
        r.outDir = "dist/lantern-run-linux";
        r.zip = "dist/lantern-run-linux.zip";
        r.target = pack::Target::Linux;
        const std::vector<std::string> args = pack::packArgs(r);
        check(hasArgPair(args, "--project", "games/lantern-run"), "the project is passed");
        check(hasArgPair(args, "--player", "build/ukiyo_player"), "the player is passed");
        check(hasArgPair(args, "--platform", "linux"), "the platform is passed");
        check(hasArgPair(args, "--out", "dist/lantern-run-linux"), "the output folder is passed");
        check(hasArgPair(args, "--zip", "dist/lantern-run-linux.zip"), "the archive is passed");
        check(hasArgPair(args, "--shaders", "build/shaders"), "the shaders are passed");
        check(args.size() == 14, "and nothing else", std::to_string(args.size()));

        r.target = pack::Target::Windows;
        r.zip.clear();
        const std::vector<std::string> win = pack::packArgs(r);
        check(hasArgPair(win, "--platform", "windows"), "the Windows target names windows");
        bool hasZip = false;
        for (const std::string& a : win) hasZip = hasZip || a == "--zip";
        check(!hasZip, "no archive flag when none was asked for");
        check(pack::targetName(pack::Target::Android) == std::string("android"), "the Android target names android");
        check(pack::defaultOutDir("games/lantern-run", pack::Target::Linux) == "dist/lantern-run-linux",
              "the default folder is derived from the project and target",
              pack::defaultOutDir("games/lantern-run", pack::Target::Linux));
    }

    // --- The manifest's version field ---------------------------------------------------------------
    {
        const fs::path root = fs::temp_directory_path() / "ukiyo_editor_tools_test";
        std::error_code ec;
        fs::remove_all(root, ec);
        const fs::path build = root / "Build/build.json";
        write(build, R"({"format":"ukiyo-build","version":1,"name":"Court","startScene":"Scenes/Court.ukscene","scenes":["Scenes/Court.ukscene"]})");
        check(pack::manifestVersion(build.string()).empty(), "a manifest with no version reads as empty");
        std::string err;
        check(pack::setManifestVersion(build.string(), "0.4.2", &err), "a version can be set", err);
        check(pack::manifestVersion(build.string()) == "0.4.2", "and reads back");
        const std::string after = read(build);
        check(after.find("\"Court\"") != std::string::npos, "the rest of the manifest is untouched");
        check(after.find("\"scenes\"") != std::string::npos, "including the scene list");
        write(root / "notabuild.json", R"({"hello":1})");
        check(!pack::setManifestVersion((root / "notabuild.json").string(), "1.0", &err),
              "a file that is not a build manifest is refused");

        // --- Localisation: the catalogues a game ships ----------------------------------------------
        write(root / "assets/locales/en.json", R"({"format":"ukiyo-locale","locale":"en",
          "strings":{"lanterns":"Lanterns","all_lit":"All five lanterns lit."}})");
        write(root / "assets/locales/ja.json", R"({"strings":{"lanterns":"灯籠"}})");
        const std::vector<std::string> codes = ukiyo::availableLocales(root.string());
        check(codes.size() == 2 && codes[0] == "en" && codes[1] == "ja", "both catalogues are found",
              std::to_string(codes.size()));
        LocaleCatalog en, ja;
        check(ukiyo::loadLocale(root.string(), "en", &en, &err), "English loads", err);
        check(ukiyo::loadLocale(root.string(), "ja", &ja, &err), "Japanese loads", err);
        check(en.tr("lanterns") == "Lanterns", "a key resolves to its text");
        check(ja.tr("lanterns") == "灯籠", "the same key is translated");
        check(ja.tr("all_lit") == "all_lit", "a missing key falls back to the key itself, not to nothing");
        check(!ukiyo::loadLocale(root.string(), "fr", &ja, &err), "a language with no catalogue does not load");
        LocaleCatalog fr = ukiyo::newLocale(en, "fr");
        check(fr.strings.size() == en.strings.size(), "a new language starts from the base language's keys");
        check(fr.tr("lanterns") == "Lanterns", "with its text, ready to be replaced");
        write(root / "assets/locales/fr.json", ukiyo::localeJson(fr));
        LocaleCatalog frBack;
        check(ukiyo::loadLocale(root.string(), "fr", &frBack, &err) && frBack.code == "fr",
              "a written catalogue reads back", err);
        fs::remove_all(root, ec);
    }

    // --- Git: porcelain into a change list ----------------------------------------------------------
    {
        const std::string porcelain =
            "## main...origin/main [ahead 1]\n"
            " M Scenes/Court.ukscene\n"
            "A  assets/new.png\n"
            "?? notes.txt\n"
            " D old.glb\n"
            "R  assets/a.glb -> assets/props/b.glb\n";
        const vcs::Status s = vcs::parseStatus(porcelain);
        check(s.ok && s.error.empty(), "a status parses");
        check(s.branch == "main", "the branch is read", s.branch);
        check(s.upstream == "origin/main", "the upstream is read", s.upstream);
        check(s.ahead && !s.behind, "ahead is read");
        check(s.changes.size() == 5, "every changed path is an entry", std::to_string(s.changes.size()));
        if (s.changes.size() == 5) {
            check(s.changes[0].path == "Scenes/Court.ukscene" && !s.changes[0].staged, "an unstaged edit");
            check(s.changes[0].kind == vcs::Change::Kind::Modified, "and its kind");
            check(s.changes[1].staged && s.changes[1].kind == vcs::Change::Kind::Added, "a staged addition");
            check(s.changes[2].kind == vcs::Change::Kind::Untracked, "an untracked file");
            check(s.changes[3].kind == vcs::Change::Kind::Deleted, "a deletion in the work tree");
            check(s.changes[4].kind == vcs::Change::Kind::Renamed && s.changes[4].path == "assets/props/b.glb" &&
                      s.changes[4].original == "assets/a.glb",
                  "a rename keeps both names");
        }
        const vcs::Status failed = vcs::parseStatus("", "fatal: not a git repository");
        check(!failed.ok && failed.error.find("not a git repository") != std::string::npos,
              "a failed command reports git's own words");
        const vcs::Status detached = vcs::parseStatus("## HEAD (no branch)\n M a.txt\n");
        check(detached.detached && detached.branch == "HEAD", "a detached head is recognised");

        check(vcs::commitArgs("hello")[0] == "commit" && vcs::commitArgs("hello")[2] == "hello",
              "the commit command passes the message as one argument");
        check(vcs::pushArgs("origin", "main").size() == 3, "the push command names its remote and branch");
        check(vcs::defaultCommitMessage(s.changes).find("Court.ukscene") != std::string::npos,
              "the default message names what changed");
        check(vcs::defaultCommitMessage({}) == "Editor snapshot", "an empty change set still has a message");

        const vcs::Commit c = vcs::parseLog("abc123\x1f" "Ada\x1f" "2 days ago\x1f" "A commit subject\n")[0];
        check(c.hash == "abc123" && c.author == "Ada" && c.subject == "A commit subject", "a log line parses");
    }

    std::printf("editor_tools_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
