#include "engine/terrain.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <fstream>
#include <iterator>

namespace ukiyo {

namespace {

float clampIndex(int v, uint32_t size) {
    return float(std::clamp(v, 0, int(size) - 1));
}

}  // namespace

float Heightmap::heightAt(int x, int z) const {
    const int cx = std::clamp(x, 0, int(size) - 1);
    const int cz = std::clamp(z, 0, int(size) - 1);
    return samples[size_t(cz) * size + cx] * heightScale;
}

float Heightmap::heightAtMetres(float x, float z) const {
    if (!valid()) return 0.0f;
    const float fx = std::clamp(x / cellSize, 0.0f, float(size - 1));
    const float fz = std::clamp(z / cellSize, 0.0f, float(size - 1));
    const int x0 = int(std::floor(fx)), z0 = int(std::floor(fz));
    const int x1 = std::min(x0 + 1, int(size) - 1), z1 = std::min(z0 + 1, int(size) - 1);
    const float tx = fx - float(x0), tz = fz - float(z0);
    const float h0 = heightAt(x0, z0) * (1.0f - tx) + heightAt(x1, z0) * tx;
    const float h1 = heightAt(x0, z1) * (1.0f - tx) + heightAt(x1, z1) * tx;
    return h0 * (1.0f - tz) + h1 * tz;
}

glm::vec3 Heightmap::normalAt(int x, int z) const {
    const float hL = heightAt(x - 1, z), hR = heightAt(x + 1, z);
    const float hD = heightAt(x, z - 1), hU = heightAt(x, z + 1);
    // Central differences. A clamped edge spans one sample, so it is half as steep as an interior step.
    const float dx = float(std::min(x + 1, int(size) - 1) - std::max(x - 1, 0)) * cellSize;
    const float dz = float(std::min(z + 1, int(size) - 1) - std::max(z - 1, 0)) * cellSize;
    const glm::vec3 n(-(hR - hL) / std::max(dx, 1e-6f), 1.0f, -(hU - hD) / std::max(dz, 1e-6f));
    return glm::normalize(n);
}

uint32_t chunksPerSide(const Heightmap& map, uint32_t cells) {
    return map.valid() ? chunksForSize(map.size, cells) : 0;
}

ChunkMesh buildChunk(const Heightmap& map, uint32_t cx, uint32_t cz, uint32_t cells) {
    ChunkMesh mesh;
    const uint32_t n = chunksPerSide(map, cells);
    if (n == 0 || cx >= n || cz >= n) return mesh;

    const uint32_t side = cells + 1;   // samples per side of this chunk
    const int x0 = int(cx * cells), z0 = int(cz * cells);
    mesh.positions.reserve(size_t(side) * side);
    mesh.normals.reserve(size_t(side) * side);
    mesh.uvs.reserve(size_t(side) * side);
    mesh.boundsMin = glm::vec3(FLT_MAX);
    mesh.boundsMax = glm::vec3(-FLT_MAX);
    for (uint32_t j = 0; j < side; ++j) {
        for (uint32_t i = 0; i < side; ++i) {
            const int gx = x0 + int(i), gz = z0 + int(j);
            const glm::vec3 p(float(gx) * map.cellSize, map.heightAt(gx, gz), float(gz) * map.cellSize);
            mesh.positions.push_back(p);
            mesh.normals.push_back(map.normalAt(gx, gz));
            mesh.uvs.emplace_back(float(i) / float(cells), float(j) / float(cells));
            mesh.boundsMin = glm::min(mesh.boundsMin, p);
            mesh.boundsMax = glm::max(mesh.boundsMax, p);
        }
    }
    mesh.indices.reserve(size_t(cells) * cells * 6);
    for (uint32_t j = 0; j < cells; ++j) {
        for (uint32_t i = 0; i < cells; ++i) {
            const uint32_t a = j * side + i, b = a + 1, c = a + side, d = c + 1;
            // Counter-clockwise seen from above (+Y), so the top faces up.
            mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
        }
    }
    return mesh;
}

bool loadHeightmapU16(const std::string& path, uint32_t size, Heightmap* out, std::string* error) {
    if (!out) {
        if (error) *error = "no heightmap to fill";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "could not read " + path;
        return false;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t want = size_t(size) * size * 2;
    if (size < 2 || bytes.size() != want) {
        if (error) *error = path + " has " + std::to_string(bytes.size()) + " bytes, expected " + std::to_string(want);
        return false;
    }
    Heightmap m;
    m.size = size;
    m.samples.resize(size_t(size) * size);
    for (size_t k = 0; k < m.samples.size(); ++k) {
        const unsigned v = unsigned(bytes[2 * k]) | (unsigned(bytes[2 * k + 1]) << 8);
        m.samples[k] = float(v) / 65535.0f;
    }
    m.cellSize = out->cellSize;         // the caller's cell size and height scale are kept
    m.heightScale = out->heightScale;
    *out = std::move(m);
    return true;
}

bool saveHeightmapU16(const std::string& path, const Heightmap& map, std::string* error) {
    if (!map.valid()) {
        if (error) *error = "heightmap is empty";
        return false;
    }
    std::vector<unsigned char> bytes(map.samples.size() * 2);
    for (size_t k = 0; k < map.samples.size(); ++k) {
        const unsigned v = unsigned(std::lround(std::clamp(map.samples[k], 0.0f, 1.0f) * 65535.0f));
        bytes[2 * k] = static_cast<unsigned char>(v & 0xFF);
        bytes[2 * k + 1] = static_cast<unsigned char>(v >> 8);
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        if (error) *error = "could not write " + path;
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    return bool(out);
}

} // namespace ukiyo
