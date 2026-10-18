// Resource pack ("resources.pak") and the read hook that serves files from it.
//
// A pack holds the files of a project folder (models, textures, scripts) in one file. Entries are keyed by their
// path relative to the folder that holds the pack, with '/' separators. Once a pack is mounted, every read that
// goes through readBytes() is served from the pack when the file is in it, and from disk otherwise.
//
// Format (little-endian): "UKPAK1\0\0", u32 version (1), u32 count, then per entry: u32 name length, name bytes,
// u64 offset, u64 size. Offsets are from the start of the file. Data is stored uncompressed.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ukiyo::vfs {

// Packs the files under `dirs` (folders relative to `root`) into `pakPath`. `pakPath` should sit in `root`, so the
// keys match what mountPak() expects. Entries are written in sorted order, so the same input gives the same bytes.
bool buildPak(const std::string& root, const std::vector<std::string>& dirs, const std::string& pakPath,
              std::string* error, int* fileCount = nullptr);

// Mounts a pack. Its keys are relative to the folder that holds `pakPath`. Mounting a second pack replaces the first.
bool mountPak(const std::string& pakPath, std::string* error);
void unmountPak();
bool pakMounted();

// True when `path` (absolute, or relative to the working directory) is an entry of the mounted pack, or exists on disk.
bool exists(const std::string& path);
// Reads a whole file, from the pack when it is mounted and the file is in it, else from disk.
bool readBytes(const std::string& path, std::vector<uint8_t>& out);

} // namespace ukiyo::vfs
