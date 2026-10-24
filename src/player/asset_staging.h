// First-launch asset staging, shared by the phone and the test.
//
// An APK is a zip: its assets cannot be opened by path, so the Android player copies them out once, on first launch,
// into the app's internal storage, and every project path resolves from there. The list comes from the package itself
// rather than from a directory walk, because the NDK's asset API cannot tell a directory from a file without trying
// to open it, and a failed open then looks exactly like an empty directory.
//
// The staging loop lives here, parameterised by where the bytes come from, so tests/android_stage_test.cpp replays
// the exact loop a phone runs — the list parsing, the parent directories, the partial-write refusal — against a
// folder standing in for the APK, without an emulator in the building.
//
//   int staged = ukiyo::staging::stage(&ctx, readCallback, root, &error);   // count, or -1 with *error set

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <direct.h>          // MSVC spells mkdir "_mkdir" and keeps it out of sys/stat.h
#else
#include <sys/stat.h>
#endif

namespace ukiyo::staging {

namespace detail {
#if defined(_WIN32)
inline void makeOneDir(const char* path) { _mkdir(path); }
#else
inline void makeOneDir(const char* path) { ::mkdir(path, 0755); }
#endif
}  // namespace detail

inline void makeParents(const std::string& path) {
    // Every directory above `path`, not `path` itself.
    for (size_t i = 1; i < path.size(); ++i) {
        if (path[i] != '/') continue;
        detail::makeOneDir(path.substr(0, i).c_str());   // EEXIST is the normal case after the first file in a folder
    }
}

inline bool writeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    makeParents(path);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    return written == bytes.size();
}

// `ctx` goes through to the reader untouched (on Android it is the AAssetManager; a test hands a folder). The reader
// is anything callable as bool(Ctx*, const std::string&, std::vector<uint8_t>*) -- a function pointer from the NDK
// side, a lambda from the test.
template <typename Ctx, typename ReadBytes>
int stage(Ctx* ctx, ReadBytes readBytes, const std::string& root, std::string* error) {
    std::vector<uint8_t> list;
    if (!readBytes(ctx, "files.txt", &list)) {
        if (error) *error = "files.txt is missing from the package";
        return -1;
    }
    const std::string names(reinterpret_cast<const char*>(list.data()), list.size());
    size_t start = 0;
    int copied = 0;
    while (start < names.size()) {
        const size_t end = names.find('\n', start);
        std::string name = names.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? names.size() : end + 1;
        while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
        if (name.empty()) continue;
        std::vector<uint8_t> bytes;
        if (!readBytes(ctx, name, &bytes)) {
            if (error) *error = "asset " + name + " could not be read";
            return -1;
        }
        if (!writeFile(root + "/" + name, bytes)) {
            if (error) *error = "could not write " + root + "/" + name;
            return -1;
        }
        ++copied;
    }
    return copied;
}

} // namespace ukiyo::staging
