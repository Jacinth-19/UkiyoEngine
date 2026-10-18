#include "engine/vfs.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <utility>

namespace ukiyo::vfs {
namespace fs = std::filesystem;
namespace {

constexpr char kMagic[8] = {'U', 'K', 'P', 'A', 'K', '1', '\0', '\0'};
constexpr uint32_t kVersion = 1;

struct Entry {
    uint64_t offset = 0;
    uint64_t size = 0;
};

struct Mounted {
    bool active = false;
    fs::path pakPath;   // absolute
    fs::path root;      // folder that holds the pack, absolute and canonical
    std::map<std::string, Entry> entries;
};

Mounted& mounted() {
    static Mounted m;
    return m;
}

void putU32(std::string& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void putU64(std::string& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
bool getU32(std::istream& in, uint32_t& v) {
    unsigned char b[4];
    if (!in.read(reinterpret_cast<char*>(b), 4)) return false;
    v = 0;
    for (int i = 0; i < 4; ++i) v |= uint32_t(b[i]) << (8 * i);
    return true;
}
bool getU64(std::istream& in, uint64_t& v) {
    unsigned char b[8];
    if (!in.read(reinterpret_cast<char*>(b), 8)) return false;
    v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t(b[i]) << (8 * i);
    return true;
}

// Key for an absolute file path under the mounted root, or "" when the path is outside it.
std::string keyFor(const fs::path& absPath, const fs::path& root) {
    const fs::path rel = absPath.lexically_relative(root);
    if (rel.empty()) return {};
    const std::string first = rel.begin()->string();
    if (first == "..") return {};
    return rel.generic_string();
}

fs::path canonicalOf(const std::string& path) {
    std::error_code ec;
    fs::path p = fs::absolute(path, ec);
    if (ec) return {};
    return fs::weakly_canonical(p, ec).lexically_normal();
}

} // namespace

bool buildPak(const std::string& root, const std::vector<std::string>& dirs, const std::string& pakPath,
              std::string* error, int* fileCount) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    const fs::path rootPath = canonicalOf(root);
    if (rootPath.empty() || !fs::is_directory(rootPath)) return fail("pack root is not a folder: " + root);
    const fs::path pakAbs = canonicalOf(pakPath);

    // Collect the files first, keyed and sorted, so the output is deterministic.
    std::map<std::string, fs::path> files;
    for (const std::string& dir : dirs) {
        const fs::path d = rootPath / dir;
        if (!fs::is_directory(d)) return fail("not a folder under the pack root: " + dir);
        for (const auto& it : fs::recursive_directory_iterator(d)) {
            if (!it.is_regular_file()) continue;
            const fs::path abs = canonicalOf(it.path().string());
            if (abs == pakAbs) continue;
            const std::string key = keyFor(abs, rootPath);
            if (key.empty()) return fail("file is outside the pack root: " + it.path().string());
            files[key] = abs;
        }
    }

    // The index size is known up front, so every data offset can be written before any data is copied.
    std::vector<std::pair<std::string, fs::path>> list(files.begin(), files.end());
    uint64_t indexBytes = 0;
    for (const auto& [key, path] : list) indexBytes += 4 + key.size() + 16;
    uint64_t cursor = sizeof(kMagic) + 4 + 4 + indexBytes;

    std::ofstream file(fs::path(pakPath), std::ios::binary | std::ios::trunc);
    if (!file) return fail("cannot write " + pakPath);
    std::string head(kMagic, sizeof(kMagic));
    putU32(head, kVersion);
    putU32(head, static_cast<uint32_t>(list.size()));
    file.write(head.data(), static_cast<std::streamsize>(head.size()));
    std::vector<uint64_t> sizes;
    for (const auto& [key, path] : list) {
        std::error_code ec;
        const uint64_t size = fs::file_size(path, ec);
        if (ec) return fail("cannot read " + path.string());
        sizes.push_back(size);
        std::string rec;
        putU32(rec, static_cast<uint32_t>(key.size()));
        rec += key;
        putU64(rec, cursor);
        putU64(rec, size);
        file.write(rec.data(), static_cast<std::streamsize>(rec.size()));
        cursor += size;
    }
    for (const auto& [key, path] : list) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return fail("cannot read " + path.string());
        std::vector<char> buf(1 << 16);
        while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            file.write(buf.data(), in.gcount());
        }
    }
    file.close();
    if (!file) return fail("write failed: " + pakPath);
    if (fileCount) *fileCount = static_cast<int>(list.size());
    return true;
}

bool mountPak(const std::string& pakPath, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    const fs::path abs = canonicalOf(pakPath);
    std::ifstream in(abs, std::ios::binary);
    if (!in) return fail("cannot open pack: " + pakPath);
    char magic[sizeof(kMagic)];
    if (!in.read(magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0)
        return fail("not a ukiyo resource pack: " + pakPath);
    uint32_t version = 0, count = 0;
    if (!getU32(in, version) || version != kVersion) return fail("unsupported pack version in " + pakPath);
    if (!getU32(in, count)) return fail("truncated pack index: " + pakPath);
    std::map<std::string, Entry> entries;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t len = 0;
        if (!getU32(in, len) || len > 4096) return fail("bad pack index entry in " + pakPath);
        std::string key(len, '\0');
        if (!in.read(key.data(), len)) return fail("truncated pack index: " + pakPath);
        Entry e;
        if (!getU64(in, e.offset) || !getU64(in, e.size)) return fail("truncated pack index: " + pakPath);
        entries[key] = e;
    }
    Mounted& m = mounted();
    m.active = true;
    m.pakPath = abs;
    m.root = abs.parent_path();
    m.entries = std::move(entries);
    return true;
}

void unmountPak() { mounted() = Mounted{}; }

bool pakMounted() { return mounted().active; }

// Looks up an entry for `path`. Returns false when the pack is not mounted or does not hold the file.
static bool findEntry(const std::string& path, Entry& out) {
    const Mounted& m = mounted();
    if (!m.active) return false;
    const std::string key = keyFor(canonicalOf(path), m.root);
    if (key.empty()) return false;
    const auto it = m.entries.find(key);
    if (it == m.entries.end()) return false;
    out = it->second;
    return true;
}

bool exists(const std::string& path) {
    Entry e;
    if (findEntry(path, e)) return true;
    std::error_code ec;
    return fs::exists(path, ec);
}

bool readBytes(const std::string& path, std::vector<uint8_t>& out) {
    Entry e;
    if (findEntry(path, e)) {
        std::ifstream in(mounted().pakPath, std::ios::binary);
        if (!in) return false;
        in.seekg(static_cast<std::streamoff>(e.offset));
        out.resize(static_cast<size_t>(e.size));
        return static_cast<bool>(in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(e.size)));
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff n = in.tellg();
    in.seekg(0, std::ios::beg);
    if (n < 0) return false;
    out.resize(static_cast<size_t>(n));
    return n == 0 || static_cast<bool>(in.read(reinterpret_cast<char*>(out.data()), n));
}

} // namespace ukiyo::vfs
