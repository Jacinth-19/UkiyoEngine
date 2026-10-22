// The first game's geometry, written as GLB files by code rather than shipped as binaries nobody can regenerate.
//
// A game engine's first sample has a chicken-and-egg problem: it needs art to look like a game, and art is the one thing
// a code repository does not have. These are the four pieces Lantern Run needs — a stone platform, a torii gate, a stone
// lantern and a paper lantern — built from boxes, cylinders and cones and written as glTF 2.0 binary. Every vertex is
// generated here, so the committed assets are exactly the output of this file (`--check` proves it), and changing a
// proportion is a one-line edit rather than a trip through a modelling program.
//
//   lantern_assets --out games/lantern-run/assets          write the four files
//   lantern_assets --out games/lantern-run/assets --check  write nothing; fail if the files on disk differ
//
// The writer is deliberately small: one buffer, one mesh with N primitives, flat normals, one material per primitive.
// That is everything the engine's loader reads (positions, normals, indices, base colour, emissive) and nothing else.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::json;
using glm::vec3;
using glm::vec4;

struct Material {
    std::string name;
    vec4 base{1.0f};
    vec3 emissive{0.0f};
    float roughness = 0.85f;
    float metallic = 0.0f;
};

// One primitive: a triangle list with flat normals and one material.
struct Part {
    std::vector<vec3> pos;
    std::vector<vec3> nrm;
    std::vector<uint32_t> idx;
    int material = 0;
};

struct Asset {
    std::string name;
    std::vector<Material> materials;
    std::vector<Part> parts;
};

// ---- Geometry ---------------------------------------------------------------------------------
//
// Everything is built as flat-shaded triangles: each face gets its own vertices, which is what a stone lantern wants and
// what keeps the writer free of a normal-smoothing pass.

void append(Part& dst, const Part& src) {
    const uint32_t base = static_cast<uint32_t>(dst.pos.size());
    dst.pos.insert(dst.pos.end(), src.pos.begin(), src.pos.end());
    dst.nrm.insert(dst.nrm.end(), src.nrm.begin(), src.nrm.end());
    for (uint32_t i : src.idx) dst.idx.push_back(base + i);
}

void translate(Part& p, vec3 t) {
    for (vec3& v : p.pos) v += t;
}

void addTriangle(Part& p, vec3 a, vec3 b, vec3 c) {
    const vec3 n = glm::normalize(glm::cross(b - a, c - a));
    const uint32_t base = static_cast<uint32_t>(p.pos.size());
    for (const vec3& v : {a, b, c}) {
        p.pos.push_back(v);
        p.nrm.push_back(n);
    }
    p.idx.push_back(base);
    p.idx.push_back(base + 1);
    p.idx.push_back(base + 2);
}

// A quad a-b-c-d, counter-clockwise seen from its front.
void addQuad(Part& p, vec3 a, vec3 b, vec3 c, vec3 d) {
    addTriangle(p, a, b, c);
    addTriangle(p, a, c, d);
}

void addBox(Part& p, vec3 lo, vec3 hi) {
    const vec3 c[8] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {lo.x, lo.y, hi.z},
                       {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
    addQuad(p, c[0], c[3], c[2], c[1]);   // bottom
    addQuad(p, c[4], c[5], c[6], c[7]);   // top
    addQuad(p, c[0], c[1], c[5], c[4]);   // -z
    addQuad(p, c[2], c[3], c[7], c[6]);   // +z
    addQuad(p, c[1], c[2], c[6], c[5]);   // +x
    addQuad(p, c[3], c[0], c[4], c[7]);   // -x
}

// A capped cylinder around the y axis, from y0 to y1.
void addCylinder(Part& p, float radius, float y0, float y1, int segments) {
    const float tau = 6.28318530718f;
    for (int i = 0; i < segments; ++i) {
        const float a0 = tau * float(i) / float(segments);
        const float a1 = tau * float(i + 1) / float(segments);
        const vec3 v0(std::cos(a0) * radius, 0.0f, std::sin(a0) * radius);
        const vec3 v1(std::cos(a1) * radius, 0.0f, std::sin(a1) * radius);
        addQuad(p, vec3(v0.x, y0, v0.z), vec3(v1.x, y0, v1.z), vec3(v1.x, y1, v1.z), vec3(v0.x, y1, v0.z));
        addTriangle(p, vec3(0.0f, y1, 0.0f), vec3(v0.x, y1, v0.z), vec3(v1.x, y1, v1.z));   // top
        addTriangle(p, vec3(0.0f, y0, 0.0f), vec3(v1.x, y0, v1.z), vec3(v0.x, y0, v0.z));   // bottom
    }
}

// A square pyramid: a roof. `half` is the base half-width, y0 the eaves, y1 the ridge.
void addPyramid(Part& p, float half, float y0, float y1) {
    const vec3 e[4] = {{-half, y0, -half}, {half, y0, -half}, {half, y0, half}, {-half, y0, half}};
    const vec3 apex(0.0f, y1, 0.0f);
    for (int i = 0; i < 4; ++i) addTriangle(p, e[i], e[(i + 1) % 4], apex);
    addQuad(p, e[0], e[3], e[2], e[1]);
}

// A flat ring (the platform's lit rim), from r0 to r1 at height y.
void addRing(Part& p, float r0, float r1, float y, int segments) {
    const float tau = 6.28318530718f;
    for (int i = 0; i < segments; ++i) {
        const float a0 = tau * float(i) / float(segments);
        const float a1 = tau * float(i + 1) / float(segments);
        const vec3 i0(std::cos(a0) * r0, y, std::sin(a0) * r0);
        const vec3 i1(std::cos(a1) * r0, y, std::sin(a1) * r0);
        const vec3 o0(std::cos(a0) * r1, y, std::sin(a0) * r1);
        const vec3 o1(std::cos(a1) * r1, y, std::sin(a1) * r1);
        addQuad(p, i0, i1, o1, o0);
    }
}

// ---- The four assets --------------------------------------------------------------------------

Asset stonePlatform() {
    Asset a;
    a.name = "Platform";
    a.materials.push_back({"Stone", vec4(0.30f, 0.31f, 0.36f, 1.0f), vec3(0.0f), 0.92f, 0.0f});
    a.materials.push_back({"Moss", vec4(0.22f, 0.32f, 0.24f, 1.0f), vec3(0.0f), 0.95f, 0.0f});
    Part stone;
    stone.material = 0;
    addCylinder(stone, 7.0f, -0.6f, 0.0f, 64);
    a.parts.push_back(stone);
    // The moss inlay sits a millimetre above the stone so the two do not fight for the same depth.
    Part moss;
    moss.material = 1;
    addRing(moss, 6.2f, 6.95f, 0.001f, 64);
    a.parts.push_back(moss);
    return a;
}

Asset torii() {
    Asset a;
    a.name = "Torii";
    a.materials.push_back({"Vermilion", vec4(0.62f, 0.13f, 0.08f, 1.0f), vec3(0.0f), 0.6f, 0.0f});
    a.materials.push_back({"Lacquer", vec4(0.10f, 0.09f, 0.10f, 1.0f), vec3(0.0f), 0.45f, 0.0f});
    Part red;
    red.material = 0;
    // Two posts, built around the origin and moved out to the beam's ends.
    for (float x : {-2.6f, 2.6f}) {
        Part post;
        post.material = 0;
        addCylinder(post, 0.26f, 0.0f, 5.0f, 16);
        translate(post, vec3(x, 0.0f, 0.0f));
        append(red, post);
    }
    addBox(red, vec3(-4.2f, 4.55f, -0.22f), vec3(4.2f, 4.95f, 0.22f));    // the tie beam
    addBox(red, vec3(-0.35f, 4.95f, -0.18f), vec3(0.35f, 5.35f, 0.18f));  // the strut
    a.parts.push_back(red);
    Part dark;
    dark.material = 1;
    addBox(dark, vec3(-4.9f, 5.35f, -0.35f), vec3(4.9f, 5.75f, 0.35f));   // the top lintel
    a.parts.push_back(dark);
    return a;
}

Asset stoneLantern() {
    Asset a;
    a.name = "StoneLantern";
    a.materials.push_back({"Granite", vec4(0.34f, 0.35f, 0.38f, 1.0f), vec3(0.0f), 0.9f, 0.0f});
    a.materials.push_back({"Paper", vec4(1.0f, 0.86f, 0.62f, 1.0f), vec3(1.0f, 0.55f, 0.18f), 0.7f, 0.0f});
    Part stone;
    stone.material = 0;
    addBox(stone, vec3(-0.34f, 0.0f, -0.34f), vec3(0.34f, 0.16f, 0.34f));      // base
    addCylinder(stone, 0.13f, 0.16f, 0.9f, 12);                                // post
    addBox(stone, vec3(-0.30f, 0.9f, -0.30f), vec3(0.30f, 1.0f, 0.30f));       // shelf
    addPyramid(stone, 0.46f, 1.52f, 1.78f);                                     // roof
    a.parts.push_back(stone);
    Part paper;
    paper.material = 1;
    addBox(paper, vec3(-0.22f, 1.0f, -0.22f), vec3(0.22f, 1.52f, 0.22f));      // the light box
    a.parts.push_back(paper);
    return a;
}

Asset paperLantern() {
    Asset a;
    a.name = "PaperLantern";
    a.materials.push_back({"Paper", vec4(1.0f, 0.90f, 0.72f, 1.0f), vec3(1.0f, 0.62f, 0.26f), 0.8f, 0.0f});
    a.materials.push_back({"Ribs", vec4(0.35f, 0.18f, 0.08f, 1.0f), vec3(0.0f), 0.7f, 0.0f});
    Part paper;
    paper.material = 0;
    addCylinder(paper, 0.26f, -0.34f, 0.34f, 20);
    a.parts.push_back(paper);
    Part ribs;
    ribs.material = 1;
    addCylinder(ribs, 0.28f, 0.30f, 0.36f, 20);    // top cap
    addCylinder(ribs, 0.28f, -0.36f, -0.30f, 20);  // bottom cap
    a.parts.push_back(ribs);
    return a;
}

// ---- The GLB writer ---------------------------------------------------------------------------
//
// glTF 2.0 binary: a 12-byte header, a JSON chunk, a binary chunk. Every accessor here is tightly packed and aligned to
// four bytes, which is the only layout the spec requires and the only one this writer produces.

void appendBytes(std::vector<uint8_t>& out, const void* data, size_t bytes) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + bytes);
}

void padTo4(std::vector<uint8_t>& out, uint8_t fill) {
    while (out.size() % 4 != 0) out.push_back(fill);
}

// Positions and normals of one primitive, interleaved by buffer view (two views, not one).
void writePrimitiveBytes(const Part& part, std::vector<uint8_t>& bin, json& views, json& accessors, int& viewIndex,
                         int& accessorIndex, size_t& byteOffset) {
    // Three views per primitive: positions, normals, indices. Written in that order so a reader that streams the file
    // touches it once.
    const size_t posBytes = part.pos.size() * sizeof(vec3);
    const size_t nrmBytes = part.nrm.size() * sizeof(vec3);
    const size_t idxBytes = part.idx.size() * sizeof(uint32_t);

    views.push_back({{"buffer", 0}, {"byteOffset", byteOffset}, {"byteLength", posBytes}, {"target", 34962}});
    const int posView = viewIndex++;
    appendBytes(bin, part.pos.data(), posBytes);
    byteOffset += posBytes;

    views.push_back({{"buffer", 0}, {"byteOffset", byteOffset}, {"byteLength", nrmBytes}, {"target", 34962}});
    const int nrmView = viewIndex++;
    appendBytes(bin, part.nrm.data(), nrmBytes);
    byteOffset += nrmBytes;

    padTo4(bin, 0);
    byteOffset = bin.size();
    views.push_back({{"buffer", 0}, {"byteOffset", byteOffset}, {"byteLength", idxBytes}, {"target", 34963}});
    const int idxView = viewIndex++;
    appendBytes(bin, part.idx.data(), idxBytes);
    byteOffset += idxBytes;
    padTo4(bin, 0);
    byteOffset = bin.size();

    vec3 lo = part.pos.front(), hi = part.pos.front();
    for (const vec3& p : part.pos) {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    accessors.push_back({{"bufferView", posView},
                         {"componentType", 5126},
                         {"count", part.pos.size()},
                         {"type", "VEC3"},
                         {"min", {lo.x, lo.y, lo.z}},
                         {"max", {hi.x, hi.y, hi.z}}});
    ++accessorIndex;   // POSITION: written just above
    accessors.push_back({{"bufferView", nrmView}, {"componentType", 5126}, {"count", part.nrm.size()}, {"type", "VEC3"}});
    ++accessorIndex;   // NORMAL
    accessors.push_back({{"bufferView", idxView}, {"componentType", 5125}, {"count", part.idx.size()}, {"type", "SCALAR"}});
    ++accessorIndex;   // indices
}

std::vector<uint8_t> buildGlb(const Asset& asset) {
    std::vector<uint8_t> bin;
    json views = json::array();
    json accessors = json::array();
    json primitives = json::array();
    int viewIndex = 0, accessorIndex = 0;
    size_t byteOffset = 0;

    for (const Part& part : asset.parts) {
        if (part.pos.empty() || part.idx.empty()) continue;
        const int firstAccessor = accessorIndex;
        writePrimitiveBytes(part, bin, views, accessors, viewIndex, accessorIndex, byteOffset);
        primitives.push_back({{"attributes", {{"POSITION", firstAccessor}, {"NORMAL", firstAccessor + 1}}},
                              {"indices", firstAccessor + 2},
                              {"material", part.material},
                              {"mode", 4}});
    }

    json materials = json::array();
    for (const Material& m : asset.materials) {
        json j = {{"name", m.name},
                  {"pbrMetallicRoughness",
                   {{"baseColorFactor", {m.base.r, m.base.g, m.base.b, m.base.a}},
                    {"metallicFactor", m.metallic},
                    {"roughnessFactor", m.roughness}}},
                  {"doubleSided", true}};
        if (glm::length(m.emissive) > 0.0f) j["emissiveFactor"] = {m.emissive.r, m.emissive.g, m.emissive.b};
        materials.push_back(j);
    }

    json root;
    root["asset"] = {{"version", "2.0"}, {"generator", "UkiyoEngine lantern_assets"}};
    root["scene"] = 0;
    root["scenes"] = json::array({json{{"name", asset.name}, {"nodes", {0}}}});
    root["nodes"] = json::array({json{{"name", asset.name}, {"mesh", 0}}});
    root["meshes"] = json::array({json{{"name", asset.name}, {"primitives", primitives}}});
    root["materials"] = materials;
    root["accessors"] = accessors;
    root["bufferViews"] = views;
    root["buffers"] = json::array({json{{"byteLength", bin.size()}}});

    std::string text = root.dump();
    std::vector<uint8_t> jsonChunk(text.begin(), text.end());
    padTo4(jsonChunk, ' ');

    std::vector<uint8_t> out;
    const uint32_t total = 12 + 8 + static_cast<uint32_t>(jsonChunk.size()) + 8 + static_cast<uint32_t>(bin.size());
    auto put32 = [&](uint32_t v) { appendBytes(out, &v, sizeof(v)); };
    put32(0x46546C67u);   // "glTF"
    put32(2);
    put32(total);
    put32(static_cast<uint32_t>(jsonChunk.size()));
    put32(0x4E4F534Au);   // "JSON"
    out.insert(out.end(), jsonChunk.begin(), jsonChunk.end());
    put32(static_cast<uint32_t>(bin.size()));
    put32(0x004E4942u);   // "BIN\0"
    out.insert(out.end(), bin.begin(), bin.end());
    while (out.size() % 4 != 0) out.push_back(0);
    return out;
}

enum class WriteResult { Unchanged, Written, Failed };

WriteResult writeIfChanged(const std::string& path, const std::vector<uint8_t>& bytes, bool checkOnly) {
    std::vector<uint8_t> old;
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::rewind(f);
        old.resize(static_cast<size_t>(size > 0 ? size : 0));
        if (!old.empty()) {
            const size_t got = std::fread(old.data(), 1, old.size(), f);
            old.resize(got);
        }
        std::fclose(f);
    }
    if (old == bytes) {
        std::printf("  %s: unchanged (%zu bytes)\n", path.c_str(), bytes.size());
        return WriteResult::Unchanged;
    }
    if (checkOnly) {
        std::fprintf(stderr, "  %s: differs from what this tool writes\n", path.c_str());
        return WriteResult::Failed;
    }
    std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "  %s: cannot write\n", path.c_str());
        return WriteResult::Failed;
    }
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    std::printf("  %s: wrote %zu bytes\n", path.c_str(), bytes.size());
    return WriteResult::Written;
}

} // namespace

int main(int argc, char** argv) {
    std::string out = "games/lantern-run/assets";
    bool check = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--check") check = true;
        else if (a == "--help" || a == "-h") {
            std::printf("usage: lantern_assets [--out DIR] [--check]\n");
            return 0;
        }
    }

    struct Entry {
        const char* file;
        Asset (*build)();
    };
    const Entry entries[] = {{"platform.glb", stonePlatform},
                             {"torii.glb", torii},
                             {"stone_lantern.glb", stoneLantern},
                             {"paper_lantern.glb", paperLantern}};
    std::printf("lantern_assets: %s\n", check ? "checking" : "writing");
    int changed = 0, failed = 0;
    size_t triangles = 0;
    for (const Entry& e : entries) {
        const Asset asset = e.build();
        size_t tris = 0;
        for (const Part& p : asset.parts) tris += p.idx.size() / 3;
        triangles += tris;
        const std::vector<uint8_t> glb = buildGlb(asset);
        const WriteResult r = writeIfChanged(out + "/" + e.file, glb, check);
        if (r == WriteResult::Written) ++changed;
        if (r == WriteResult::Failed) ++failed;
        if (r != WriteResult::Failed)
            std::printf("  %s: %zu triangles, %zu material(s)\n", e.file, tris, asset.materials.size());
    }
    std::printf("lantern_assets: %zu triangles in %zu files, %d %s%s\n", triangles,
                sizeof(entries) / sizeof(entries[0]), check ? failed : changed,
                check ? "file(s) out of date" : "file(s) written", failed ? " (FAILED)" : "");
    return failed == 0 ? 0 : 1;
}
