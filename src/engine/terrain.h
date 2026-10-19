// Terrain data (item 18, step 1): a heightmap, its chunk meshes, and the raw 16-bit file format. Pure CPU code with no
// GPU or physics dependency, so the tests can run without a device. Drawing and colliding use these chunks elsewhere.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace ukiyo {

// A tile blends up to this many layers (item 18, step 2).
constexpr uint32_t kTerrainMaxLayers = 8;

// One surface layer. A layer covers a band of normalised heights and a band of slopes, with soft edges of the given
// widths. Where a layer is painted off, its weight is zero. The colour and roughness are used until per-layer textures
// arrive.
struct TerrainLayer {
    std::string name = "layer";
    glm::vec3 colour{0.4f, 0.4f, 0.35f};       // albedo, linear
    float roughness = 0.8f;                    // 0 smooth to 1 rough; a smooth surface gets a crisper toon edge
    float heightMin = 0.0f, heightMax = 1.0f;  // normalised height band, 0 to 1 of heightScale
    float heightFade = 0.1f;                   // soft edge width of the height band, normalised
    float slopeMin = 0.0f, slopeMax = 90.0f;   // slope band in degrees
    float slopeFade = 5.0f;                    // soft edge width of the slope band, degrees
    float tileMetres = 8.0f;                   // metres of ground per texture repeat
    // Image files, absolute once the scene is read. Empty means the layer has no such map: the diffuse is white, the
    // normal is flat, the occlusion is full, and the roughness is the constant above.
    std::string diffuseFile;
    std::string normalFile;
    std::string roughnessFile;                 // the red channel is the roughness
    std::string aoFile;                        // the red channel is ambient occlusion; packed in the normal atlas
};

// The layers a new tile starts with: grass on the low gentle ground, rock on steep slopes, and pale rock at the top.
std::vector<TerrainLayer> defaultTerrainLayers();

// Foliage (item 18, step 3). Each type scatters one small static mesh over the tile. Two shapes are built in for now:
// a crossed-quad grass tuft and a cone (a tree). Model files are not used yet.
constexpr uint32_t kFoliageMaxTypes = 4;
enum class FoliageShape : uint8_t { Tuft, Cone };

struct FoliageType {
    std::string name = "grass";
    FoliageShape shape = FoliageShape::Tuft;
    float density = 0.5f;                        // chance per metre cell, 0 to 1, before the density map
    std::string densityMap;                      // image; its red channel scales the density. Empty means uniform
    float heightMin = 0.0f, heightMax = 1.0f;    // normalised height band
    float slopeMax = 35.0f;                      // degrees
    float scaleMin = 0.8f, scaleMax = 1.2f;      // size multiplier of the shape (a tuft is 1 m tall, a cone 2 m)
    uint32_t seed = 1;
    float fadeStart = 60.0f, fadeEnd = 90.0f;    // metres from the camera: full size inside fadeStart, gone past fadeEnd
    glm::vec3 tint{0.30f, 0.50f, 0.20f};         // albedo, linear
};

// The look and behaviour of one grass layer on a terrain tile. Defaults are a meadow: short green blades that thin
// out with distance, sway in a light wind, and grow only where the first terrain layer has coverage.
struct GrassSettings {
    bool enabled = false;
    // Candidates per square metre. The scatter puts one jittered candidate in every cell of a square grid whose side
    // is 1 / sqrt(density) metres, so this is the ceiling before the detail map and the layer masks thin it.
    float density = 4.0f;
    // Image whose red channel scales the density. Empty means the detail map is full everywhere. Resampled to the
    // heightmap's sample grid, like a foliage density map.
    std::string densityMap;

    // Where the grass grows, from the heightmap. Normalised height, and slope in degrees.
    float heightMin = 0.0f, heightMax = 1.0f;
    float slopeMax = 35.0f;

    // Blade size. The mesh is built at these metres and then scaled per instance.
    float height = 0.44f;          // metres to the tip of the tallest blade
    float width = 0.018f;          // metres across the base of one blade
    uint32_t blades = 8;           // blades per tuft
    uint32_t segments = 4;         // quads along each blade; more segments bend more smoothly
    float scaleMin = 0.75f, scaleMax = 1.35f;
    uint32_t seed = 3;
    // 0 keeps every blade upright, 1 lays it along the ground normal. Anything between blends the two.
    float groundAlign = 0.55f;

    // Distance bands, in metres from the camera. Inside `bandNear` every blade is drawn; the density falls smoothly
    // to half at `bandMid` and to nothing at `cullDistance`. The subset is chosen by a hash of the blade's ground
    // position, so it is stable frame to frame and a near view is always a superset of a far view: blades appear as
    // the camera approaches instead of flickering.
    float bandNear = 24.0f;
    float bandMid = 48.0f;
    float cullDistance = 95.0f;

    // Terrain layers the grass grows on, and layers that clear it. Bit k is layer k of the tile. The include test is
    // the largest weight among the included layers, soft-edged at `includeThreshold`; the exclude test hides a blade
    // once an excluded layer reaches `excludeThreshold`.
    uint32_t includeLayers = 0xFFFFFFFFu;
    float includeThreshold = 0.35f;
    uint32_t excludeLayers = 0u;
    float excludeThreshold = 0.5f;

    // Wind, in the reference's terms: one low-frequency gust wave that bends the whole field, one small fast flutter
    // per blade. `windStrength` is the tip deflection in metres at full strength.
    glm::vec2 windDir{0.85f, 0.53f};
    float windStrength = 0.22f;
    float windFrequency = 0.45f;   // cycles per second
    float windScale = 0.11f;       // radians per metre across the field, so gusts travel as bands
    float flutter = 0.05f;         // metres of fast per-blade sway

    // Albedo at the root and at the tip, linear. The tip is also what the sun shines through.
    glm::vec3 baseColour{0.095f, 0.165f, 0.052f};
    glm::vec3 tipColour{0.46f, 0.60f, 0.235f};
    float translucency = 0.5f;     // 0 to 1: how much sun comes through a lit tip
    float tipDarkening = 0.35f;    // fake AO: 0 leaves the root at full colour, 1 darkens it to black
};

// A detail overlay for a whole tile: one small texture tiled in world space over the blended layers, plus the
// large-scale colour variation that breaks up the repeat of a single layer. Production terrains lean on this when a
// layer's texture tiles every few metres; without it a field is visibly a grid under the camera.
struct TerrainDetail {
    std::string file;        // image; empty means no detail overlay (the shader binds white)
    float metres = 1.7f;     // size of one repeat of `file`, in metres
    float strength = 0.55f;  // 0 to 1: how much the overlay modulates the albedo near the camera
    float macro = 0.30f;     // 0 to 1: how strongly the large-scale variation tints the albedo
};

// What a terrain instance stores in the scene file (item 18). The instance's position is the tile origin.
struct TerrainSettings {
    std::string heightmap;      // raw 16-bit file, absolute once the scene is read
    uint32_t size = 1025;       // samples per side
    float cellSize = 1.0f;      // metres between samples
    float heightScale = 10.0f;  // metres for a normalised height of 1
    std::vector<TerrainLayer> layers = defaultTerrainLayers();   // 1 to kTerrainMaxLayers entries
    std::vector<FoliageType> foliage;                            // 0 to kFoliageMaxTypes entries
    // Layer mask images (item 18, step 4), absolute once the scene is read. Each is RGBA: its four channels are layers
    // 0 to 3 (or 4 to 7 for the second file). Empty means no layer masks were saved.
    std::vector<std::string> maskFiles;
    TerrainDetail detail;
    // Grass (item 19). Off unless a scene or the editor turns it on.
    GrassSettings grass;
};

// Chunk size used for drawing and colliders, in cells per side. 1024 cells divide into 16 chunks.
constexpr uint32_t kTerrainChunkCells = 64;

// A square grid of height samples. `size` samples per side, so size - 1 cells. Heights are stored normalised to
// [0, 1] and scaled to metres by heightScale. Samples are row major: index = z * size + x.
struct Heightmap {
    uint32_t size = 0;
    float cellSize = 1.0f;      // metres between neighbouring samples
    float heightScale = 1.0f;   // metres for a normalised height of 1
    std::vector<float> samples; // size * size values in [0, 1]

    bool valid() const { return size >= 2 && samples.size() == size_t(size) * size; }
    // Height in metres at sample (x, z). Indices are clamped to the grid.
    float heightAt(int x, int z) const;
    // Height in metres at a point given in metres from the grid origin, bilinear between samples.
    float heightAtMetres(float x, float z) const;
    // Unit normal at sample (x, z) from central differences, clamped at the edges.
    glm::vec3 normalAt(int x, int z) const;
};

// One chunk of a heightmap: a square of `cells` by `cells` quads. Chunks do not share vertices. Neighbours share
// their edge positions exactly, because both read the same samples.
struct ChunkMesh {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uvs;     // 0 to 1 across the chunk
    std::vector<uint32_t> indices;  // two triangles per cell, counter-clockwise seen from above
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
};

// Number of chunks per side, or 0 when the grid does not divide into whole chunks of `cells` cells.
uint32_t chunksPerSide(const Heightmap& map, uint32_t cells);

// Chunks per side for a tile of `size` samples, from the size alone. 0 when the size does not split into whole chunks.
inline uint32_t chunksForSize(uint32_t size, uint32_t cells) {
    if (size < 2 || cells == 0) return 0;
    const uint32_t span = size - 1;
    return span % cells == 0 ? span / cells : 0;
}

// Builds chunk (cx, cz). Returns an empty mesh when the chunk is outside the grid or the map is not valid.
ChunkMesh buildChunk(const Heightmap& map, uint32_t cx, uint32_t cz, uint32_t cells);

// Reads a raw file of size * size little-endian 16-bit values (65535 is height 1). `size` must match the file. The
// cellSize and heightScale already in *out are kept. Returns false with *error set when the file cannot be read or
// its length is wrong.
bool loadHeightmapU16(const std::string& path, uint32_t size, Heightmap* out, std::string* error);
bool saveHeightmapU16(const std::string& path, const Heightmap& map, std::string* error);

} // namespace ukiyo
