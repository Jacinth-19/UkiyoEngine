// cgltf file hooks: cgltf reads external buffers and the .gltf file itself through these, so they come from the
// resource pack when one is mounted. Set them on cgltf_options.file before cgltf_parse_file / cgltf_load_buffers.
#pragma once

#include <cstdlib>
#include <cstring>

#include "cgltf.h"
#include "engine/vfs.h"

namespace ukiyo::vfs {

inline cgltf_result cgltfRead(const struct cgltf_memory_options*, const struct cgltf_file_options*, const char* path,
                              cgltf_size* size, void** data) {
    std::vector<uint8_t> bytes;
    if (!readBytes(path, bytes)) return cgltf_result_file_not_found;
    void* copy = std::malloc(bytes.empty() ? 1 : bytes.size());
    if (!copy) return cgltf_result_out_of_memory;
    if (!bytes.empty()) std::memcpy(copy, bytes.data(), bytes.size());
    *size = bytes.size();
    *data = copy;
    return cgltf_result_success;
}

inline void cgltfRelease(const struct cgltf_memory_options*, const struct cgltf_file_options*, void* data,
                         cgltf_size) {
    std::free(data);
}

// Sets the read and release hooks on a cgltf options struct.
inline void useForCgltf(cgltf_options& opts) {
    opts.file.read = cgltfRead;
    opts.file.release = cgltfRelease;
}

} // namespace ukiyo::vfs
