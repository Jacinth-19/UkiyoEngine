// ukiyo_pack: assembles a game folder someone else can run, and a zip to hand them.
//
//   ukiyo_pack --project games/lantern-run --player build/ukiyo_player
//              --platform linux --out dist/lantern-run-linux --zip dist/lantern-run-linux.zip
//
// Everything the packer does is in src/pack/pack.cpp, where it is tested without a filesystem full of fixtures; this is
// the command line around it, with the one thing a build needs that a library cannot do: print what it did.
#include <cstdio>
#include <cstring>
#include <string>

#include "pack/pack.h"

int main(int argc, char** argv) {
    std::string project = "games/lantern-run";
    std::string player;
    std::string platform = "linux";
    std::string out;
    std::string zip;
    std::string shaders;
    std::string root = ".";
    bool verify = true;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string* dst) {
            if (i + 1 < argc) *dst = argv[++i];
        };
        if (a == "--project") next(&project);
        else if (a == "--player") next(&player);
        else if (a == "--platform") next(&platform);
        else if (a == "--out") next(&out);
        else if (a == "--zip") next(&zip);
        else if (a == "--shaders") next(&shaders);
        else if (a == "--root") next(&root);
        else if (a == "--no-verify") verify = false;
        else if (a == "--quiet") quiet = true;
        else if (a == "--help" || a == "-h") {
            std::printf("usage: ukiyo_pack --player <binary> --out <dir> [--project DIR] [--root DIR]\n"
                        "                  [--platform linux|windows] [--shaders DIR] [--zip FILE] [--no-verify]\n");
            return 0;
        } else {
            std::fprintf(stderr, "ukiyo_pack: unknown argument %s\n", a.c_str());
            return 2;
        }
    }
    if (player.empty() || out.empty()) {
        std::fprintf(stderr, "ukiyo_pack: --player and --out are required (try --help)\n");
        return 2;
    }

    ukiyo::pack::Result result;
    std::string error;
    if (!ukiyo::pack::build(root, project, player, platform, shaders, out, &result)) {
        std::fprintf(stderr, "ukiyo_pack: %s\n", result.error.c_str());
        return 1;
    }
    if (!quiet) {
        std::printf("ukiyo_pack: %s %s for %s\n", result.gameName.c_str(), result.gameVersion.c_str(),
                    platform.c_str());
        for (const ukiyo::pack::FileEntry& f : result.files)
            std::printf("  %8llu  %s\n", static_cast<unsigned long long>(f.bytes), f.path.c_str());
        std::printf("  %zu file(s), %llu bytes in %s\n", result.files.size(),
                    static_cast<unsigned long long>(result.bytes), out.c_str());
    }

    if (verify && !ukiyo::pack::verify(out, &error)) {
        std::fprintf(stderr, "ukiyo_pack: the package does not verify: %s\n", error.c_str());
        return 1;
    }
    if (!verify && !quiet) std::printf("ukiyo_pack: verification skipped\n");

    if (!zip.empty()) {
        if (!ukiyo::pack::writeZip(out, zip, &error)) {
            std::fprintf(stderr, "ukiyo_pack: %s\n", error.c_str());
            return 1;
        }
        if (!quiet) std::printf("ukiyo_pack: wrote %s\n", zip.c_str());
    }
    return 0;
}
