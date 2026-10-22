// Shipping a game: turning a project directory plus a player binary into a folder someone else can run.
//
// The player already runs a project in place — `ukiyo_player --project games/lantern-run --build Build/build.json` — so
// the only thing between a project and a build is the set of files that has to travel with it. That set is not "the
// project folder": a project may name a font in editor/res or a model in assets/, both outside its own directory, and
// those references are relative (`../../assets/alice.vrm`). The package therefore keeps the layout the references were
// written against — a game directory at its repository-relative path, the shared folders beside it — and the launcher
// runs the player with `--project <that path>`. No file is rewritten, so the packaged game and the one the editor runs
// are the same bytes.
//
// What a package contains:
//   ukiyo_player(.exe)      a copy of the player binary for the target platform
//   run.sh / run.bat        the launcher, with the project path baked in
//   package.json            the manifest: every file, its size and its CRC-32
//   <project>/**            the project directory, unchanged
//   <referenced files>      anything outside it that the manifest or the scenes name (font, models, sounds)
//
// pack::verify() reads a package back and checks every byte, and pack::writeZip() archives one without losing that
// verifiability. Both are tested in tests/pack_test.cpp, because a packer that silently drops a file ships a game that
// only works on the machine it was built on.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ukiyo {
namespace pack {

struct FileEntry {
    std::string path;      // forward slashes, relative to the package root
    uint64_t bytes = 0;
    uint32_t crc = 0;
};

struct Result {
    bool ok = false;
    std::string error;
    std::vector<FileEntry> files;   // in the order they were written, manifest excluded
    uint64_t bytes = 0;
    std::string projectPath;        // the project's path inside the package, for the launcher
    std::string gameName;           // from the build manifest, for the log and package.json
    std::string gameVersion;
};

// Every file a project needs: the project directory's own tree, plus the font and every model, script and sound its
// manifest and scenes name, resolved against `root`. Paths are root-relative and use forward slashes. A reference that
// leaves `root`, or names a file that does not exist, is an error: a package with a missing asset is worse than no
// package, because it fails on the player's machine instead of the builder's.
bool collect(const std::string& root, const std::string& projectDir, std::vector<std::string>* files,
             std::string* error);

// Assembles the package in `outDir`. `playerBinary` is copied in and named after the platform
// (`ukiyo_player.exe` on windows). `shadersDir` is the compiled shader tree (build/shaders: gl/ and spv/), copied to
// `<outDir>/shaders` and named on the launcher's command line — without it the package would only run on the machine
// whose build tree the player was compiled against. Empty skips them, which is only useful for a test. The output
// directory must not already contain a package.
bool build(const std::string& root, const std::string& projectDir, const std::string& playerBinary,
           const std::string& platform, const std::string& shadersDir, const std::string& outDir, Result* result);

// Checks a package directory against its own package.json: every listed file exists, has the recorded size and hashes
// to the recorded CRC-32, and every file on disk is listed.
bool verify(const std::string& dir, std::string* error);

// Writes a store-only (uncompressed) zip of a package directory, which is what a download is. Deflate would save a few
// megabytes on a game whose models are already compressed; storing keeps the writer honest and the archive byte-for-byte
// the same as the folder it came from.
bool writeZip(const std::string& dir, const std::string& zipPath, std::string* error);

// The CRC-32 the manifest records, and the one a zip stores. Exposed because both writers must agree on it.
uint32_t crc32(const void* data, size_t bytes);

} // namespace pack
} // namespace ukiyo
