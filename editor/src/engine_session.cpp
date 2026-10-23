#include "engine/localisation.h"
#include "engine/transform.h"
#include "engine_session.h"

#include "engine/script_project.h"

#include "anim_file.h"
#include "player/hud_layout.h"
#include "scene_file.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <cstdio>
#include <cstring>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <GLFW/glfw3.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "engine/cull.h"
#include "engine/foliage.h"
#include "engine/terrain_layers.h"
#include "engine/material_asset.h"

using namespace ukiyo;

namespace editor {

namespace {
// Default idle seed, the same one the engine uses when --idle is given without --idle-seed.
constexpr uint32_t kIdleSeed = 7;
} // namespace

bool EngineSession::init(QString* error) {
    // Point the Vulkan loader at the software driver when the environment has not set one. That driver is lavapipe,
    // a Linux package with no Windows equivalent: on Windows the loader finds the installed GPU driver by itself, and
    // the editor runs with a real device (the headless tests do not use this path).
#ifndef _WIN32
    if (!std::getenv("VK_ICD_FILENAMES"))
        setenv("VK_ICD_FILENAMES", "/usr/share/vulkan/icd.d/lvp_icd.json", 0);
#endif

    if (!glfwInit()) {
        if (error) *error = "glfwInit failed (is a display available? try xvfb-run)";
        return false;
    }
    try {
        rhi::DeviceConfig cfg;
        cfg.width = kWidth;
        cfg.height = kHeight;
        cfg.headless = true;
        device_ = rhi::createDevice(rhi::Backend::Vulkan, cfg, nullptr);
        renderer_ = std::make_unique<Renderer>(*device_);
        renderer_->setSceneLights(sceneLights_);
    } catch (const std::exception& e) {
        if (error) *error = QString::fromStdString(e.what());
        return false;
    }
    return true;
}

bool EngineSession::loadAnimLibrary(const QString& path, QString* error) {
    lib_ = AnimLibrary::load(path.toStdString());
    if (!lib_) {
        if (error) *error = "could not read animation library " + path;
        return false;
    }
    clipEdits_ = ukiyo::ClipEdits{};
    return true;
}

void EngineSession::setClipEdits(const ukiyo::ClipEdits& edits) {
    clipEdits_ = edits;
    if (lib_) lib_->setClips(ukiyo::applyClipEdits(lib_->rig(), lib_->sourceClips(), clipEdits_));
}

void EngineSession::libraryData(ukiyo::SourceRig* rig, std::vector<ukiyo::AnimClip>* clips) const {
    if (rig) *rig = lib_ ? lib_->rig() : ukiyo::SourceRig{};
    if (clips) *clips = lib_ ? lib_->sourceClips() : std::vector<ukiyo::AnimClip>{};
}

namespace {

// Reads one model file and applies the editor's defaults. Returns null with `error` set on failure.
std::unique_ptr<Model> readModelFile(rhi::Device& dev, const QString& path, bool smoothNormals, bool preserveAlpha,
                                   float alphaThreshold, QString* error) {
    std::unique_ptr<Model> m;
    try {
        ukiyo::ModelLoadOptions lo;
        lo.smoothNormals = smoothNormals;
        lo.preserveAlphaTestCoverage = preserveAlpha;
        lo.alphaTestThreshold = alphaThreshold;
        m = ukiyo::loadModel(dev, path.toStdString(), lo);
    } catch (const std::exception& e) {
        if (error) *error = QString::fromStdString(e.what());
        return nullptr;
    }
    if (!m) {
        if (error) *error = "could not load " + path;
        return nullptr;
    }
    m->physics = true;
    m->armCollision = true;
    // Cluster LODs for static meshes, cached beside the file. A skinned model returns at once (its error is measured on
    // the rest pose), so the character costs nothing here; a static prop pays a few milliseconds once and then reads
    // the sidecar. A read-only asset folder simply rebuilds.
    ukiyo::buildModelVGeom(dev, *m, ukiyo::VGeomSettings{}, m->sourcePath + ".vgeom");
    return m;
}

} // namespace

glm::mat4 EngineSession::placementOf(const glm::vec3& position, const glm::vec3& eulerDeg, const glm::vec3& scale) {
    // Scale, then turn, then place. The rotation is the engine's own Euler order (ZXY, transform.h), so an instance
    // turned by a gizmo reads the same angles the Inspector shows and a script would get.
    const glm::vec3 k(std::max(std::fabs(scale.x), 0.0001f),
                      std::max(std::fabs(scale.y), 0.0001f),
                      std::max(std::fabs(scale.z), 0.0001f));
    return glm::translate(glm::mat4(1.0f), position) * ukiyo::rotationMatrix(eulerDeg) *
           glm::scale(glm::mat4(1.0f), k);
}

Model* EngineSession::activeModel() const {
    if (active_ < 0 || active_ >= static_cast<int>(instances_.size())) return nullptr;
    return instances_[active_].model.get();
}

std::string EngineSession::modelPath() const {
    if (active_ < 0 || active_ >= static_cast<int>(instances_.size())) return std::string();
    return instances_[active_].path;
}

void EngineSession::bindActive() {
    if (lib_ && activeModel()) {
        player_.bind(*activeModel(), *lib_);
        player_.startIdle(kIdleSeed);
    }
}

void EngineSession::clearInstances() {
    // Play refers to the instances it started with, so play ends first. In a switched scene that puts the
    // editor's scene back, and the scene that is about to load replaces it.
    stopPlay();
    instances_.clear();
    active_ = -1;
    // A scene that replaces the editor's scene makes the saved one stale.
    editorInstances_.clear();
    editorStart_.clear();
    editorActive_ = -1;
    editorSaved_ = false;
    applyGraphMaterials();   // no instances left, so this drops every override
}

bool EngineSession::loadModel(const QString& path, QString* error) {
    std::unique_ptr<Model> m = readModelFile(*device_, path, smoothNormals_, preserveAlphaCoverage_, alphaTestThreshold_, error);
    if (!m) return false;
    if (active_ < 0) {
        instances_.push_back(Instance{});
        active_ = 0;
    }
    // The old model's assignments refer to its nodes, so they go with it. Other instances keep theirs.
    Instance& in = instances_[active_];
    in.nodeGraph.clear();
    in.nodeMmd.clear();
    in.lodSwitches.clear();   // the new model has no levels yet
    in.terrain.reset();       // and no terrain
    in.terrainGpu.reset();
    in.model = std::move(m);
    in.path = path.toStdString();
    in.model->placement = placementOf(in.position, {in.pitchDeg, in.yawDeg, in.rollDeg}, in.scale);
    applyPlacement(*in.model);
    applyGraphMaterials();
    bindActive();
    return true;
}

bool EngineSession::addModel(const QString& path, const float position[3], float yawDeg, QString* error,
                             int* index) {
    std::unique_ptr<Model> m = takeModel(path, error);
    if (!m) return false;
    attachModel(std::move(m), path, position, yawDeg);
    if (index) *index = active_;
    return true;
}

void EngineSession::attachModel(std::unique_ptr<Model> m, const QString& path, const float position[3], float yawDeg) {
    Instance in;
    in.model = std::move(m);
    in.path = path.toStdString();
    in.position = glm::vec3(position[0], position[1], position[2]);
    in.yawDeg = yawDeg;
    in.model->placement = placementOf(in.position, {in.pitchDeg, in.yawDeg, in.rollDeg}, in.scale);
    applyPlacement(*in.model);
    instances_.push_back(std::move(in));
    active_ = static_cast<int>(instances_.size()) - 1;
    applyGraphMaterials();
    bindActive();
}

std::unique_ptr<Model> EngineSession::takeModel(const QString& path, QString* error) {
    const std::string key = path.toStdString();
    auto it = staged_.find(key);
    if (it != staged_.end()) {
        // A staged model is used once. It is only used if the file and the normals setting still match.
        const bool fresh = it->second.smoothNormals == smoothNormals_ &&
                           it->second.preserveAlphaCoverage == preserveAlphaCoverage_ &&
                           it->second.alphaTestThreshold == alphaTestThreshold_ &&
                           it->second.modified == QFileInfo(path).lastModified().toMSecsSinceEpoch();
        std::unique_ptr<Model> m = std::move(it->second.model);
        staged_.erase(it);
        if (fresh) return m;
    }
    return readModelFile(*device_, path, smoothNormals_, preserveAlphaCoverage_, alphaTestThreshold_, error);
}

bool EngineSession::preloadModel(const QString& path, QString* error) {
    if (!device_) {
        if (error) *error = QStringLiteral("no render device");
        return false;
    }
    const std::string key = path.toStdString();
    const qint64 modified = QFileInfo(path).lastModified().toMSecsSinceEpoch();
    auto it = staged_.find(key);
    if (it != staged_.end() && it->second.smoothNormals == smoothNormals_ &&
        it->second.preserveAlphaCoverage == preserveAlphaCoverage_ && it->second.alphaTestThreshold == alphaTestThreshold_ &&
        it->second.modified == modified)
        return true;
    if (it == staged_.end() && staged_.size() >= kMaxStaged) {
        if (error) *error = QStringLiteral("the preload cache is full (%1 models)").arg(kMaxStaged);
        return false;
    }
    std::unique_ptr<Model> m = readModelFile(*device_, path, smoothNormals_, preserveAlphaCoverage_, alphaTestThreshold_, error);
    if (!m) return false;
    StagedModel s;
    s.model = std::move(m);
    s.modified = modified;
    s.smoothNormals = smoothNormals_;
    s.preserveAlphaCoverage = preserveAlphaCoverage_;
    s.alphaTestThreshold = alphaTestThreshold_;
    staged_[key] = std::move(s);
    return true;
}

std::vector<EngineSession::SceneNote> EngineSession::takeSceneNotes() {
    std::vector<SceneNote> out;
    out.swap(sceneNotes_);
    return out;
}

void EngineSession::noteScene(const std::string& text, bool structural) {
    sceneNotes_.push_back(SceneNote{text, structural});
}

bool EngineSession::setActiveInstance(int index) {
    if (index < 0 || index >= static_cast<int>(instances_.size())) return false;
    if (index == active_) return true;
    active_ = index;
    bindActive();
    return true;
}

bool EngineSession::setInstancePlacement(int index, const float position[3], float yawDeg, float pitchDeg,
                                         float rollDeg, const float scale[3]) {
    if (index < 0 || index >= static_cast<int>(instances_.size())) return false;
    Instance& in = instances_[index];
    in.position = glm::vec3(position[0], position[1], position[2]);
    in.yawDeg = yawDeg;
    in.pitchDeg = pitchDeg;
    in.rollDeg = rollDeg;
    if (scale) in.scale = glm::vec3(scale[0], scale[1], scale[2]);
    in.model->placement = placementOf(in.position, {in.pitchDeg, in.yawDeg, in.rollDeg}, in.scale);
    applyPlacement(*in.model);
    return true;
}

bool EngineSession::setInstanceLink(int index, const std::string& prefab, const std::string& prefabId,
                                    const std::set<std::string>& overrides) {
    if (index < 0 || index >= static_cast<int>(instances_.size())) return false;
    Instance& in = instances_[index];
    in.prefab = prefab;
    in.prefabId = prefabId;
    in.overrides = overrides;
    return true;
}

bool EngineSession::setInstanceName(int index, const std::string& name) {
    if (index < 0 || index >= static_cast<int>(instances_.size())) return false;
    instances_[index].name = name;
    return true;
}

bool EngineSession::setInstanceTerrain(int index, const std::optional<ukiyo::TerrainSettings>& terrain, QString* error) {
    if (index < 0 || index >= static_cast<int>(instances_.size())) {
        if (error) *error = QStringLiteral("no such object");
        return false;
    }
    Instance& in = instances_[index];
    if (!terrain) {
        in.terrain.reset();
        in.terrainGpu.reset();
        return true;
    }
    if (!device_) {
        if (error) *error = QStringLiteral("no render device");
        return false;
    }
    if (ukiyo::chunksForSize(terrain->size, ukiyo::kTerrainChunkCells) == 0) {
        if (error) *error = QStringLiteral("a terrain is 65, 129, ..., 1025 samples per side (64 cells per chunk)");
        return false;
    }
    if (terrain->layers.empty() || terrain->layers.size() > ukiyo::kTerrainMaxLayers) {
        if (error) *error = QStringLiteral("a terrain has 1 to %1 layers").arg(ukiyo::kTerrainMaxLayers);
        return false;
    }
    if (terrain->foliage.size() > ukiyo::kFoliageMaxTypes) {
        if (error) *error = QStringLiteral("a terrain has at most %1 foliage types").arg(ukiyo::kFoliageMaxTypes);
        return false;
    }
    TerrainEditState* st = ensureTerrainState(*terrain, error);
    if (!st) return false;
    std::unique_ptr<ukiyo::TerrainGpu> gpu;
    if (!buildTerrainGpu(*terrain, *st, &gpu, error)) return false;
    in.terrain = terrain;
    in.terrainGpu = std::move(gpu);
    return true;
}

// The in-memory state of a tile. A heightmap is read from its file the first time it is seen, and the masks come from
// the files named in the settings. A later call with the same path returns the state already there, with any edits.
// Reads the layer mask images named by the settings. Fills `layerPaint` only when every file is read.
static bool readLayerMasks(const ukiyo::TerrainSettings& ts, std::vector<uint8_t>* layerPaint, std::string* err) {
    const size_t samples = size_t(ts.size) * ts.size;
    std::vector<uint8_t> paint;
    if (!ts.maskFiles.empty()) {
        paint.assign(samples * ukiyo::kTerrainMaxLayers, 255);
        for (size_t m = 0; m < ts.maskFiles.size() && m < 2; ++m) {
            uint32_t w = 0, h = 0;
            std::vector<uint8_t> rgba;
            if (!ukiyo::loadRgba8(ts.maskFiles[m], &w, &h, &rgba, err) || w != ts.size || h != ts.size) {
                if (err && err->empty()) *err = "a layer mask must match the tile size";
                return false;
            }
            for (size_t s = 0; s < samples; ++s)
                for (size_t c = 0; c < 4; ++c) paint[s * ukiyo::kTerrainMaxLayers + m * 4 + c] = rgba[s * 4 + c];
        }
    }
    *layerPaint = std::move(paint);
    return true;
}

// Reads the density image of one foliage type. An empty `file` means the type has no density image.
static bool readDensity(const std::string& file, uint32_t size, std::vector<uint8_t>* field, std::string* err) {
    field->clear();
    if (file.empty()) return true;
    return ukiyo::loadDensityField(file, size, field, err);
}

EngineSession::TerrainEditState* EngineSession::ensureTerrainState(const ukiyo::TerrainSettings& ts, QString* error) {
    auto found = terrainEdits_.find(ts.heightmap);
    if (found != terrainEdits_.end()) {
        TerrainEditState& held = found->second;
        if (held.map.size != ts.size) {
            if (error) *error = QStringLiteral("the heightmap of this tile has a different size from its scene entry");
            return nullptr;
        }
        // A mask image the settings name anew (for example, a density image picked in the scene) is read again, and
        // the edit is refused when the image is missing. The held edits stay as they are otherwise.
        std::string loadErr;
        // An empty list is an older snapshot (undo restores one from before a save), so it keeps the held mask.
        if (!ts.maskFiles.empty() && ts.maskFiles != held.maskSource) {
            std::vector<uint8_t> paint;
            if (!readLayerMasks(ts, &paint, &loadErr)) {
                if (error) *error = QString::fromStdString(loadErr);
                return nullptr;
            }
            held.layerPaint = std::move(paint);
            held.layerDirty = false;
            held.maskSource = ts.maskFiles;
        }
        for (size_t t = 0; t < ts.foliage.size() && t < held.densitySource.size(); ++t) {
            if (ts.foliage[t].densityMap.empty() || ts.foliage[t].densityMap == held.densitySource[t]) continue;
            std::vector<uint8_t> field;
            if (!readDensity(ts.foliage[t].densityMap, ts.size, &field, &loadErr)) {
                if (error) *error = QString::fromStdString(loadErr);
                return nullptr;
            }
            held.density[t] = std::move(field);
            held.densityDirty[t] = 0;
            held.densitySource[t] = ts.foliage[t].densityMap;
        }
        if (ts.grass.densityMap != held.grassDensitySource) {
            std::vector<uint8_t> field;
            if (!readDensity(ts.grass.densityMap, ts.size, &field, &loadErr)) {
                if (error) *error = QString::fromStdString(loadErr);
                return nullptr;
            }
            held.grassDensity = std::move(field);
            held.grassDensitySource = ts.grass.densityMap;
        }
        return &held;
    }
    TerrainEditState st;
    st.map.size = ts.size;
    st.map.cellSize = ts.cellSize;
    st.map.heightScale = ts.heightScale;
    std::string loadErr;
    if (!ukiyo::loadHeightmapU16(ts.heightmap, ts.size, &st.map, &loadErr)) {
        if (error) *error = QString::fromStdString(loadErr);
        return nullptr;
    }
    if (!readLayerMasks(ts, &st.layerPaint, &loadErr)) {
        if (error) *error = QString::fromStdString(loadErr);
        return nullptr;
    }
    st.maskSource = ts.maskFiles;
    st.density.resize(ukiyo::kFoliageMaxTypes);
    st.densityDirty.assign(ukiyo::kFoliageMaxTypes, 0);
    st.densitySource.assign(ukiyo::kFoliageMaxTypes, std::string());
    for (size_t t = 0; t < ts.foliage.size(); ++t) {
        if (ts.foliage[t].densityMap.empty()) continue;
        if (!readDensity(ts.foliage[t].densityMap, ts.size, &st.density[t], &loadErr)) {
            if (error) *error = QString::fromStdString(loadErr);
            return nullptr;
        }
        st.densitySource[t] = ts.foliage[t].densityMap;
    }
    if (!readDensity(ts.grass.densityMap, ts.size, &st.grassDensity, &loadErr)) {
        if (error) *error = QString::fromStdString(loadErr);
        return nullptr;
    }
    st.grassDensitySource = ts.grass.densityMap;
    return &terrainEdits_.emplace(ts.heightmap, std::move(st)).first->second;
}

// The GPU copy of a tile, built from its settings and its state. The layers and foliage types come from the settings.
bool EngineSession::buildTerrainGpu(const ukiyo::TerrainSettings& ts, const TerrainEditState& st,
                                    std::unique_ptr<ukiyo::TerrainGpu>* out, QString* error) {
    auto gpu = std::make_unique<ukiyo::TerrainGpu>();
    std::string uploadErr;
    if (!ukiyo::uploadTerrain(*device_, st.map, ukiyo::kTerrainChunkCells, ts.layers, st.layerPaint, ts.detail, gpu.get(),
                              &uploadErr)) {
        if (error) *error = QString::fromStdString(uploadErr);
        return false;
    }
    std::vector<std::vector<uint8_t>> densities(ts.foliage.size());
    for (size_t t = 0; t < ts.foliage.size() && t < st.density.size(); ++t) densities[t] = st.density[t];
    if (!ukiyo::uploadFoliage(*device_, ts.foliage, densities, gpu.get(), &uploadErr)) {
        if (error) *error = QString::fromStdString(uploadErr);
        return false;
    }
    // Grass (item 19): the scatter reads the same layer masks the terrain shader blends, so blades grow on the layers
    // the settings include and clear off the ones they exclude.
    if (!ukiyo::uploadGrass(*device_, ts.layers, st.layerPaint, ts.grass, st.grassDensity, gpu.get(), &uploadErr)) {
        if (error) *error = QString::fromStdString(uploadErr);
        return false;
    }
    *out = std::move(gpu);
    return true;
}

// Rebuilds the GPU copy of every instance that shows this heightmap.
void EngineSession::refreshTerrainTiles(const std::string& path) {
    auto it = terrainEdits_.find(path);
    if (it == terrainEdits_.end()) return;
    for (Instance& in : instances_) {
        if (!in.terrain || in.terrain->heightmap != path) continue;
        std::unique_ptr<ukiyo::TerrainGpu> gpu;
        if (buildTerrainGpu(*in.terrain, it->second, &gpu, nullptr)) in.terrainGpu = std::move(gpu);
    }
    terrainUploads_.erase(path);
}

int EngineSession::pickTerrain(glm::vec3 origin, glm::vec3 dir, glm::vec3* worldHit) const {
    int best = -1;
    float bestDist = 0.0f;
    for (size_t i = 0; i < instances_.size(); ++i) {
        const Instance& in = instances_[i];
        if (!in.terrainGpu) continue;
        glm::vec3 hit;
        if (!ukiyo::raycastHeightmap(in.terrainGpu->map, origin - glm::vec3(in.position[0], in.position[1], in.position[2]),
                                     dir, 5000.0f, &hit))
            continue;
        const glm::vec3 world = hit + glm::vec3(in.position[0], in.position[1], in.position[2]);
        const float d = glm::distance(origin, world);
        if (best < 0 || d < bestDist) {
            best = static_cast<int>(i);
            bestDist = d;
            if (worldHit) *worldHit = world;
        }
    }
    return best;
}

bool EngineSession::terrainDab(int index, glm::vec3 worldHit, const TerrainBrushSettings& b, TerrainEdit* out, QString* error) {
    if (index < 0 || index >= static_cast<int>(instances_.size()) || !instances_[index].terrain) {
        if (error) *error = QStringLiteral("the brush needs a terrain tile under the cursor");
        return false;
    }
    const Instance& in = instances_[index];
    auto found = terrainEdits_.find(in.terrain->heightmap);
    if (found == terrainEdits_.end()) {
        if (error) *error = QStringLiteral("the tile is not loaded");
        return false;
    }
    TerrainEditState& st = found->second;
    const ukiyo::TerrainSettings& ts = *in.terrain;
    const glm::vec3 local = worldHit - glm::vec3(in.position[0], in.position[1], in.position[2]);

    ukiyo::BrushDab d;
    d.centre = glm::vec2(local.x, local.z);
    d.radius = std::max(0.01f, b.radius);
    d.falloff = std::clamp(b.falloff, 0.0f, 1.0f);
    d.strength = std::clamp(b.strength, -1.0f, 1.0f);
    const float cell = ts.cellSize;
    TerrainEdit e;
    e.path = QString::fromStdString(ts.heightmap);
    switch (b.tool) {
    case TerrainTool::Noise: {
        const float amp = b.amplitude / std::max(ts.heightScale, 1e-3f);
        const float freq = cell / std::max(1.0f, b.wavelength);
        e.target = 0;
        e.heights = ukiyo::heightDab(st.map, d, [&](int x, int z, float h) {
            return h + amp * ukiyo::fractalNoise(x * freq, z * freq, 11u, 4);
        });
        break;
    }
    case TerrainTool::Terrace: {
        const uint32_t steps = std::max(1u, b.steps);
        e.target = 0;
        e.heights = ukiyo::heightDab(st.map, d, [&](int, int, float h) { return ukiyo::terraceLevel(h, steps); });
        break;
    }
    case TerrainTool::Stamp: {
        const std::string file = b.stampFile.toStdString();
        if (!terrainStampLoaded_ || file != terrainStampFile_) {
            std::string serr;
            ukiyo::Stamp s = ukiyo::domeStamp(64);
            if (!file.empty() && !ukiyo::loadStamp(file, &s, &serr)) {
                if (error) *error = QString::fromStdString(serr);
                return false;
            }
            terrainStamp_ = s;
            terrainStampFile_ = file;
            terrainStampLoaded_ = true;
        }
        const float amp = b.amplitude / std::max(ts.heightScale, 1e-3f);
        e.target = 0;
        e.heights = ukiyo::heightDab(st.map, d, [&](int x, int z, float h) {
            return h + amp * ukiyo::stampAt(terrainStamp_, d, x * cell, z * cell);
        });
        break;
    }
    case TerrainTool::LayerMask:
    case TerrainTool::Density: {
        const bool layer = b.tool == TerrainTool::LayerMask;
        const uint32_t channels = layer ? ukiyo::kTerrainMaxLayers : 1;
        const int channel = layer ? std::clamp(b.channel, 0, int(ukiyo::kTerrainMaxLayers) - 1)
                                  : std::clamp(b.channel, 0, int(ukiyo::kFoliageMaxTypes) - 1);
        if (!layer && static_cast<size_t>(channel) >= ts.foliage.size()) {
            if (error) *error = QStringLiteral("add a foliage type first, then paint its density");
            return false;
        }
        std::vector<uint8_t>& field = layer ? st.layerPaint : st.density[channel];
        const size_t samples = size_t(st.map.size) * st.map.size;
        if (field.size() != samples * channels) {
            // The first paint makes the mask. A layer mask starts with every layer allowed, a density mask starts empty.
            field.assign(samples * channels, layer ? uint8_t(255) : uint8_t(0));
            e.createdMask = true;
            if (layer) st.layerDirty = true;
            else st.densityDirty[channel] = 1;
        }
        e.target = layer ? 1 : 2;
        e.channel = channel;
        e.paint = ukiyo::paintDab(field, st.map.size, channels, layer ? channel : 0, d, b.add ? 255 : 0, cell);
        break;
    }
    case TerrainTool::None:
        if (error) *error = QStringLiteral("no terrain tool is selected");
        return false;
    }
    *out = e;
    return applyTerrainEdit(e, true, false, error);
}

bool EngineSession::terrainErode(int index, const ukiyo::ErosionSettings& s, TerrainEdit* out, QString* error) {
    if (index < 0 || index >= static_cast<int>(instances_.size()) || !instances_[index].terrain) {
        if (error) *error = QStringLiteral("select a terrain tile to erode it");
        return false;
    }
    auto found = terrainEdits_.find(instances_[index].terrain->heightmap);
    if (found == terrainEdits_.end()) {
        if (error) *error = QStringLiteral("the tile is not loaded");
        return false;
    }
    TerrainEdit e;
    e.path = QString::fromStdString(instances_[index].terrain->heightmap);
    e.target = 0;
    e.heights = ukiyo::erodeHeightmap(found->second.map, s);
    *out = e;
    return applyTerrainEdit(e, true, true, error);
}

// Applies one edit to the in-memory tile: the `after` values to redo, or the `before` values to undo. The GPU copy is
// rebuilt now when `upload` is set, otherwise at flushTerrainUploads.
bool EngineSession::applyTerrainEdit(const TerrainEdit& e, bool useAfter, bool upload, QString* error) {
    auto found = terrainEdits_.find(e.path.toStdString());
    if (found == terrainEdits_.end()) {
        if (error) *error = QStringLiteral("the edited tile is not loaded");
        return false;
    }
    TerrainEditState& st = found->second;
    const size_t samples = size_t(st.map.size) * st.map.size;
    if (e.target == 0) {
        ukiyo::applyHeightEdits(st.map, e.heights, useAfter);
        st.heightsDirty = true;
    } else if (e.target == 1 || e.target == 2) {
        const bool layer = e.target == 1;
        const uint32_t channels = layer ? ukiyo::kTerrainMaxLayers : 1;
        const int channel = std::clamp(e.channel, 0, int(layer ? ukiyo::kTerrainMaxLayers : ukiyo::kFoliageMaxTypes) - 1);
        std::vector<uint8_t>& field = layer ? st.layerPaint : st.density[channel];
        if (field.size() != samples * channels) {
            field.assign(samples * channels, layer ? uint8_t(255) : uint8_t(0));   // the mask exists again on redo
        }
        ukiyo::applyPaintEdits(field, e.paint, useAfter);
        if (!useAfter && e.createdMask) field.clear();   // undoing the first paint removes the mask
        if (layer) st.layerDirty = true;
        else st.densityDirty[channel] = 1;
    }
    if (upload) refreshTerrainTiles(e.path.toStdString());
    else terrainUploads_.insert(e.path.toStdString());
    return true;
}

void EngineSession::flushTerrainUploads() {
    const std::set<std::string> pending = terrainUploads_;
    for (const std::string& path : pending) refreshTerrainTiles(path);
    terrainUploads_.clear();
}

bool EngineSession::saveTerrainEdits(QString* error) {
    for (auto& [path, st] : terrainEdits_) {
        if (st.heightsDirty) {
            std::string serr;
            if (!ukiyo::saveHeightmapU16(path, st.map, &serr)) {
                if (error) *error = QString::fromStdString(serr);
                return false;
            }
            st.heightsDirty = false;
        }
        const QFileInfo fi(QString::fromStdString(path));
        const QString base = fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName();
        const uint32_t size = st.map.size;
        if (st.layerDirty && st.layerPaint.empty()) {
            // Undo removed the mask. Drop its files, so a reopen does not bring the undone paint back.
            for (uint32_t m = 0; m < 2; ++m) QFile::remove(base + QStringLiteral("_layers%1.png").arg(m));
            for (Instance& in : instances_)
                if (in.terrain && in.terrain->heightmap == path) in.terrain->maskFiles.clear();
            st.maskSource.clear();
            st.layerDirty = false;
        }
        if (st.layerDirty && !st.layerPaint.empty()) {
            std::vector<std::string> files;
            for (uint32_t m = 0; m < 2; ++m) {
                std::vector<uint8_t> rgba(size_t(size) * size * 4);
                for (size_t s = 0; s < size_t(size) * size; ++s)
                    for (size_t c = 0; c < 4; ++c) rgba[s * 4 + c] = st.layerPaint[s * ukiyo::kTerrainMaxLayers + m * 4 + c];
                const QString file = base + QStringLiteral("_layers%1.png").arg(m);
                if (!QImage(rgba.data(), int(size), int(size), int(size) * 4, QImage::Format_RGBA8888).copy().save(file)) {
                    if (error) *error = QStringLiteral("could not write %1").arg(file);
                    return false;
                }
                files.push_back(file.toStdString());
            }
            for (Instance& in : instances_)
                if (in.terrain && in.terrain->heightmap == path) in.terrain->maskFiles = files;
            st.maskSource = files;
            st.layerDirty = false;
        }
        for (size_t t = 0; t < st.density.size(); ++t) {
            if (!st.densityDirty[t]) continue;
            const QString file = base + QStringLiteral("_density%1.png").arg(t);
            if (st.density[t].empty()) {
                // Undo removed the density mask. Drop its file and its scene key, as with the layer masks.
                QFile::remove(file);
                for (Instance& in : instances_)
                    if (in.terrain && in.terrain->heightmap == path && t < in.terrain->foliage.size())
                        in.terrain->foliage[t].densityMap.clear();
                st.densitySource[t].clear();
                st.densityDirty[t] = 0;
                continue;
            }
            QImage img(int(size), int(size), QImage::Format_Grayscale8);
            for (uint32_t z = 0; z < size; ++z)
                std::memcpy(img.scanLine(int(z)), st.density[t].data() + size_t(z) * size, size);
            if (!img.save(file)) {
                if (error) *error = QStringLiteral("could not write %1").arg(file);
                return false;
            }
            for (Instance& in : instances_)
                if (in.terrain && in.terrain->heightmap == path && t < in.terrain->foliage.size())
                    in.terrain->foliage[t].densityMap = file.toStdString();
            st.densitySource[t] = file.toStdString();
            st.densityDirty[t] = 0;
        }
    }
    return true;
}

void EngineSession::clearTerrainEdits() {
    terrainEdits_.clear();
    terrainUploads_.clear();
}

std::vector<float> EngineSession::terrainHeights(int index) const {
    if (index < 0 || index >= static_cast<int>(instances_.size()) || !instances_[index].terrainGpu) return {};
    return instances_[index].terrainGpu->map.samples;
}

int EngineSession::terrainFoliageCount(int index) const {
    if (index < 0 || index >= static_cast<int>(instances_.size()) || !instances_[index].terrainGpu) return 0;
    int total = 0;
    for (const ukiyo::FoliageBatchGpu& b : instances_[index].terrainGpu->foliage) total += static_cast<int>(b.count);
    return total;
}

float EngineSession::terrainHeightAt(int index, float x, float z) const {
    if (index < 0 || index >= static_cast<int>(instances_.size()) || !instances_[index].terrainGpu) return 0.0f;
    return instances_[index].terrainGpu->map.heightAtMetres(x, z);
}

bool EngineSession::setInstanceLod(int index, const std::vector<float>& switches, QString* error) {
    if (index < 0 || index >= static_cast<int>(instances_.size())) {
        if (error) *error = QStringLiteral("no such object");
        return false;
    }
    if (!device_) {
        if (error) *error = QStringLiteral("no render device");
        return false;
    }
    Instance& in = instances_[index];
    in.lodSwitches = ukiyo::normalizeLodSwitches(switches);
    ukiyo::buildModelLods(*device_, *in.model, in.lodSwitches);
    return true;
}

EngineSession::PlacedModel EngineSession::instanceInfo(int index) const {
    PlacedModel p;
    if (index < 0 || index >= static_cast<int>(instances_.size())) return p;
    const Instance& in = instances_[index];
    p.path = in.path;
    p.name = in.name;
    p.lodSwitches = in.lodSwitches;
    p.terrain = in.terrain;
    p.position[0] = in.position.x;
    p.position[1] = in.position.y;
    p.position[2] = in.position.z;
    p.yawDeg = in.yawDeg;
    p.pitchDeg = in.pitchDeg;
    p.rollDeg = in.rollDeg;
    for (int k = 0; k < 3; ++k) p.scale[k] = in.scale[k];
    p.prefab = in.prefab;
    p.prefabId = in.prefabId;
    p.overrides = in.overrides;
    return p;
}

std::vector<std::string> EngineSession::clipNames() const {
    std::vector<std::string> names;
    if (lib_)
        for (const AnimClip& c : lib_->clips()) names.push_back(c.name);
    return names;
}

float EngineSession::clipDuration(const std::string& name) const {
    if (lib_)
        for (const AnimClip& c : lib_->clips())
            if (c.name == name) return c.duration;
    return 0.0f;
}

void EngineSession::playClip(const std::string& name, float fade, float startFraction) {
    if (!player_.bound()) return;
    if (name.empty())
        player_.startIdle(kIdleSeed);
    else if (lib_ && lib_->find(name) >= 0)
        player_.play(name, fade, startFraction);   // a clip name the library does not have is ignored, not thrown
}

void EngineSession::playBlend(const std::vector<std::string>& clips, const std::vector<float>& weights,
                              const std::vector<ukiyo::MotionOptions>& options, float fade) {
    if (!player_.bound()) return;
    if (fade < 0.0f) {
        player_.setBlendWeights(weights);
        return;
    }
    // A clip the library does not have is skipped, the same as playClip.
    std::vector<std::string> known;
    std::vector<float> w;
    std::vector<ukiyo::MotionOptions> opts;
    for (size_t i = 0; i < clips.size(); ++i) {
        if (!lib_ || lib_->find(clips[i]) < 0) continue;
        known.push_back(clips[i]);
        w.push_back(i < weights.size() ? weights[i] : 0.0f);
        opts.push_back(i < options.size() ? options[i] : ukiyo::MotionOptions{});
    }
    if (!known.empty()) player_.playBlend(known, w, fade, opts);
}

const std::string& EngineSession::currentClip() const {
    return player_.current();
}

glm::vec3 EngineSession::nodeWorldPosition(int node) const {
    const Model* m = activeModel();
    if (!m || node < 0 || node >= static_cast<int>(m->nodes.size())) return glm::vec3(0.0f);
    return glm::vec3(m->nodes[node].world[3]);
}

void EngineSession::setRenderSize(uint32_t w, uint32_t h) {
    if (device_) device_->resizeBackbuffer(w, h);
}

void EngineSession::shutdown() {
    stopPlay();
    particles_.reset();
    navVerts_.clear();
    navTris_.clear();
    navBaked_ = false;
    if (renderer_) renderer_->clearGraphMaterials();
    graphMats_.clear();
    whiteTex_.reset();
    instances_.clear();
    active_ = -1;
    staged_.clear();   // staged models hold GPU buffers, so they go before the device
    lib_.reset();
    renderer_.reset();
    device_.reset();
}

QString EngineSession::setNodeGraphMaterial(int node, const QString& ukmatPath) {
    if (active_ < 0) return QString();
    auto& graph = instances_[active_].nodeGraph;
    if (ukmatPath.isEmpty())
        graph.erase(node);
    else
        graph[node] = ukmatPath.toStdString();
    return applyGraphMaterials();
}

QString EngineSession::setNodeMmdToon(int node, bool on) {
    if (active_ < 0) return QString();
    auto& set = instances_[active_].nodeMmd;
    if (on)
        set.insert(node);
    else
        set.erase(node);
    return applyGraphMaterials();
}

QString EngineSession::setGradingLut(const QString& absPath) {
    if (!renderer_) return QStringLiteral("renderer is not ready");
    std::string err;
    if (!renderer_->setGradingLut(absPath.toStdString(), &err)) return QString::fromStdString(err);
    return QString();
}

QString EngineSession::refreshGraphMaterials() {
    return applyGraphMaterials();
}

GraphMaterial* EngineSession::graphMaterialFor(const std::string& ukmatPath, QString* error)
{
    const QFileInfo matInfo(QString::fromStdString(ukmatPath));
    engine::MaterialAsset mat;
    engine::ShaderAsset shader;
    std::string err;
    if (!engine::loadMaterialAsset(ukmatPath, mat, err)) {
        if (error) *error = QString::fromStdString(err);
        return nullptr;
    }
    // Shader paths are relative to the assets folder, the parent of Materials/.
    QDir assets(matInfo.absolutePath());
    assets.cdUp();
    const std::string shaderPath = assets.filePath(QString::fromStdString(mat.shaderPath)).toStdString();
    if (!engine::loadShaderAsset(shaderPath, shader, err)) {
        if (error) *error = QString::fromStdString(err);
        return nullptr;
    }

    const qint64 matMs = matInfo.lastModified().toMSecsSinceEpoch();
    const qint64 shaderMs = QFileInfo(QString::fromStdString(shaderPath)).lastModified().toMSecsSinceEpoch();
    const std::map<int, std::string> texFiles = engine::resolveTexturePaths(shader, mat, assets.absolutePath().toStdString());
    // An edited image must rebuild the material too, so the signature covers every texture file's mtime.
    std::string texSig;
    for (const auto& [binding, path] : texFiles) {
        texSig += std::to_string(binding) + "|" + path + "|"
                + std::to_string(QFileInfo(QString::fromStdString(path)).lastModified().toMSecsSinceEpoch()) + ";";
    }
    auto it = graphMats_.find(ukmatPath);
    if (it != graphMats_.end() && it->second.matModified == matMs && it->second.shaderModified == shaderMs
        && it->second.textureSig == texSig) {
        if (error && !it->second.textureError.isEmpty()) *error = it->second.textureError;
        return it->second.material.get();
    }

    try {
        if (!whiteTex_) {
            rhi::TextureDesc td;
            const uint8_t white[4] = {255, 255, 255, 255};
            whiteTex_ = device_->createTexture(td, white);
        }
        auto gm = std::make_unique<GraphMaterial>(*device_, shader, *whiteTex_, rhi::Format::RGBA16F);
        gm->setParameters(engine::packMaterialParameters(shader, mat));
        std::string texErr;
        gm->setTextureFiles(*device_, texFiles, texErr);
        GraphEntry& entry = graphMats_[ukmatPath];
        entry.material = std::move(gm);
        entry.matModified = matMs;
        entry.shaderModified = shaderMs;
        entry.textureSig = texSig;
        entry.textureError = QString::fromStdString(texErr);
        // A missing texture leaves its slot on the white fallback. The material still draws, and the error is reported.
        if (error && !entry.textureError.isEmpty()) *error = entry.textureError;
        return entry.material.get();
    } catch (const std::exception& e) {
        if (error) *error = QString::fromStdString(ukmatPath) + ": " + QString::fromStdString(e.what());
        return nullptr;
    }
}

QString EngineSession::applyGraphMaterials() {
    if (!renderer_) return QString();
    // Clear overrides first: rebuilding a cached material below may destroy the old one.
    // Every instance's assignments are re-applied, so inactive instances keep their look.
    renderer_->clearGraphMaterials();
    renderer_->clearMmdToon();

    QString firstError;
    std::map<std::string, GraphMaterial*> resolved;
    for (const Instance& in : instances_)
        for (const auto& [node, path] : in.nodeGraph) {
            if (path.empty() || resolved.count(path)) continue;
            QString err;
            resolved[path] = graphMaterialFor(path, &err);
            if (!err.isEmpty() && firstError.isEmpty()) firstError = err;
        }
    for (const Instance& in : instances_) {
        Model* m = in.model.get();
        for (const auto& [node, path] : in.nodeGraph) {
            GraphMaterial* gm = path.empty() ? nullptr : resolved[path];
            for (const DrawItem& d : m->draws)
                if (d.node == node) renderer_->setGraphMaterial(&m->primitives[d.primitive], gm);
        }
        for (int node : in.nodeMmd)
            for (const DrawItem& d : m->draws)
                if (d.node == node) renderer_->setMmdToon(&m->primitives[d.primitive], true);
    }
    return firstError;
}

QImage EngineSession::render(const OrbitCamera& cam, float time, float dt, const RenderSettings& s) {
    if (!activeModel() || !device_) return QImage();

    // Play mode moves instances first, so the model update below sees their new placement. A scene
    // switch in that step can replace the active model, so it is looked up again after the tick.
    // The water's wave clock is the render time, so buoyancy and the drawn waves agree.
    if (world_) world_->physics.setWaterTime(time);
    if (dt > 0.0f) tickPlay(dt);
    Model* active = activeModel();
    if (!active) return QImage();

    // Same order as the engine's main loop: animation, then the model update, then the frame.
    // Every instance is updated and drawn; only the active one takes the animation player.
    std::vector<Model*> models;
    for (Instance& in : instances_) {
        if (in.model.get() == active && player_.bound()) player_.update(*in.model, time, dt);
        updateModel(*in.model, time, cam.eye());
        models.push_back(in.model.get());
    }

    if (!device_->beginFrame()) return QImage();
    renderer_->setWater(water_ ? &*water_ : nullptr);
    std::vector<TerrainDraw> terrain;
    for (const Instance& in : instances_) {
        if (!in.terrainGpu) continue;
        const glm::mat4 tile = glm::translate(glm::mat4(1.0f), in.position);
        for (const ukiyo::TerrainChunkGpu& c : in.terrainGpu->chunks) {
            TerrainDraw d;
            d.vertices = c.vertices.get();
            d.indices = c.indices.get();
            d.indexCount = c.indexCount;
            d.model = tile;
            d.boundsMin = c.boundsMin;
            d.boundsMax = c.boundsMax;
            d.layers = in.terrainGpu->layerSet.get();
            terrain.push_back(d);
        }
    }
    renderer_->setTerrain(terrain);
    // The GI pass needs the heightfield of the terrain under the camera. The first tile that has one is used; a scene
    // with several tiles gives the estimator the nearest one, which is the only one its twelve-metre rays can reach.
    {
        HeightfieldGpu field;
        float best = std::numeric_limits<float>::max();
        for (const Instance& in : instances_) {
            if (!in.terrainGpu || !in.terrainGpu->heightTexture) continue;
            // The estimator reads the tile as an axis-aligned height function of world xz, so a rotated tile would
            // light the wrong ground. Tiles are placed by translation in this editor; a yaw is not supported.
            const float yaw = std::fmod(std::fabs(in.yawDeg), 360.0f);
            if (yaw > 0.01f && yaw < 359.99f) continue;
            const float d = glm::length(glm::vec2(in.position[0], in.position[2]) - glm::vec2(cam.eye().x, cam.eye().z));
            if (d >= best) continue;
            best = d;
            field.height = in.terrainGpu->heightTexture.get();
            field.size = in.terrainGpu->map.size;
            field.cellSize = in.terrainGpu->map.cellSize;
            field.heightScale = in.terrainGpu->map.heightScale;
            field.origin = glm::vec3(in.position[0], in.position[1], in.position[2]);
        }
        renderer_->setHeightfield(field);
    }
    std::vector<FoliageDraw> foliage;
    for (const Instance& in : instances_) {
        if (!in.terrainGpu) continue;
        const glm::mat4 tile = glm::translate(glm::mat4(1.0f), in.position);
        const ukiyo::TerrainGpu& g = *in.terrainGpu;
        for (const ukiyo::FoliageBatchGpu& b : g.foliage) {
            const ukiyo::FoliageMeshGpu& mesh = g.foliageMeshes[b.type];
            FoliageDraw d;
            d.vertices = mesh.vertices.get();
            d.indices = mesh.indices.get();
            d.indexCount = mesh.indexCount;
            d.instanceCount = b.count;
            d.instances = b.set.get();
            d.model = tile;
            d.boundsMin = b.boundsMin;
            d.boundsMax = b.boundsMax;
            d.fade = b.fade;
            d.tint = b.tint;
            foliage.push_back(d);
        }
    }
    renderer_->setFoliage(foliage);
    // Grass (item 19). One draw per batch; the shader's wind and distance bands read from the push block, so a batch
    // needs nothing but its instance rows.
    std::vector<GrassDraw> grass;
    glm::vec4 pusher(0.0f);   // the active character standing in the grass: xy position, reach, push
    for (const Instance& in : instances_) {
        if (in.model.get() != active) continue;
        pusher = glm::vec4(in.position.x, in.position.z, 1.6f, 0.30f);
        break;
    }
    for (const Instance& in : instances_) {
        if (!in.terrainGpu || !in.terrain) continue;
        const glm::mat4 tile = glm::translate(glm::mat4(1.0f), in.position);
        const ukiyo::TerrainGpu& g = *in.terrainGpu;
        const ukiyo::GrassSettings& gs = in.terrain->grass;
        for (const ukiyo::GrassBatchGpu& b : g.grass.batches) {
            if (b.mesh >= g.grass.meshes.size()) continue;
            const ukiyo::GrassMeshGpu& mesh = g.grass.meshes[b.mesh];
            GrassDraw d;
            d.vertices = mesh.vertices.get();
            d.indices = mesh.indices.get();
            d.instances = b.set.get();
            d.indexCount = mesh.indexCount;
            d.instanceCount = b.count;
            d.model = tile;
            d.boundsMin = b.boundsMin;
            d.boundsMax = b.boundsMax;
            d.bands = glm::vec3(gs.bandNear, gs.bandMid, gs.cullDistance);
            d.wind = glm::vec4(gs.windStrength, gs.windFrequency, gs.windScale, gs.flutter);
            d.windDir = gs.windDir;
            d.pusher = pusher;
            grass.push_back(d);
        }
    }
    renderer_->setGrass(grass);
    renderer_->render(cam, models, time, frame_++, s);
    drawParticleOverlay(cam);
    if (playing() && hudVisible_) {
        drawPlayHud();
        // The player's layout reads the size from the canvas, so the screen is set here rather than at load time.
        hudCanvas_.setScreen(float(device_->backbufferWidth()), float(device_->backbufferHeight()));
        PlayerHudText text;
        std::string title = "Play mode";
        if (active_ >= 0 && active_ < static_cast<int>(instances_.size())) {
            const Instance& act = instances_[static_cast<size_t>(active_)];
            title = act.name.empty() ? "instance" + std::to_string(active_) : act.name;
            title += "   play mode";
        }
        text.title = title;
        char stats[160];
        std::snprintf(stats, sizeof(stats), "%u draws   %.1fk tris   %d entities", renderer_->lastDrawCalls(),
                      double(renderer_->lastTriangles()) / 1000.0, playingEntities());
        text.stats = stats;
        text.hint = "WASD walk   Space jump   Esc stop";
        if (!playWarnings_.empty()) text.warning = playWarnings_.front();
        drawPlayerHud(hudCanvas_, text, 1.0f);
        renderer_->drawHud(hudCanvas_.vertices(), device_->backbufferWidth(), device_->backbufferHeight());
    }
    device_->endFrame(true);
    std::vector<uint8_t> px = device_->takeCapture();

    const int w = static_cast<int>(device_->backbufferWidth());
    const int h = static_cast<int>(device_->backbufferHeight());
    if (px.size() < static_cast<size_t>(w) * h * 4) return QImage();
    return QImage(px.data(), w, h, QImage::Format_RGBA8888).copy();
}

// --- Game systems ---------------------------------------------------------------------------

namespace {

std::string entityName(size_t index) { return "instance" + std::to_string(index); }

} // namespace

void EngineSession::setInstanceComponents(int index, const SystemsComponents& c) {
    if (index < 0 || index >= static_cast<int>(instances_.size())) return;
    instances_[index].components = c;
}

SystemsComponents EngineSession::instanceComponents(int index) const {
    if (index < 0 || index >= static_cast<int>(instances_.size())) return SystemsComponents{};
    return instances_[index].components;
}

bool EngineSession::bakeNavMesh(QString* error, int* triangles, std::atomic<int>* progress) {
    std::vector<glm::vec3> verts;
    std::vector<uint32_t> tris;
    for (Instance& in : instances_) {
        // The rest pose is enough for navigation. updateModel also refreshes the node world matrices.
        updateModel(*in.model, 0.0f, glm::vec3(0.0f));
        worldTriangles(*in.model, &verts, &tris);
        // A terrain tile has no mesh of its own: without its chunks the bake only sees the floor under the hills, and
        // every path across a valley is a straight line at the floor's height. The collider adds the same chunks, so
        // what an agent walks on and what a body falls onto are one surface.
        if (in.terrainGpu) {
            ukiyo::appendTerrainTriangles(in.terrainGpu->map, in.position, ukiyo::kTerrainChunkCells, &verts, &tris);
        }
    }
    if (tris.empty()) {
        if (error) *error = QStringLiteral("The scene has no geometry to bake.");
        return false;
    }
    NavMesh probe;
    if (!probe.build(verts, tris, NavMesh::Settings(), progress)) {
        // Recast finds no walkable surface when every triangle is steeper than the slope limit,
        // which is the case for a character alone. The likely fix is a floor, so say that.
        if (error) *error = QStringLiteral("No walkable surface found. Add a floor or ground model to the scene.");
        return false;
    }
    navVerts_ = std::move(verts);
    navTris_ = std::move(tris);
    navBaked_ = true;
    previewNav_.reset();   // the preview's copy is of the old geometry
    if (triangles) *triangles = static_cast<int>(navTris_.size() / 3);
    return true;
}

bool EngineSession::previewPath(const glm::vec3& from, const glm::vec3& to, std::vector<glm::vec3>* path,
                                QString* error) {
    if (!navBaked_) {
        if (error) *error = QStringLiteral("Bake the navmesh first.");
        return false;
    }
    if (!previewNav_) {
        previewNav_ = std::make_unique<NavMesh>();
        if (!previewNav_->build(navVerts_, navTris_)) {
            previewNav_.reset();
            if (error) *error = QStringLiteral("The navmesh could not be built for the preview.");
            return false;
        }
    }
    if (!previewNav_->findPath(from, to, path)) {
        if (error) *error = QStringLiteral("No path: a point is off the navmesh or cannot be reached.");
        return false;
    }
    return true;
}

bool EngineSession::previewSound(const std::string& path, float volume, bool loop, QString* error) {
    if (!preview_) preview_ = std::make_unique<AudioEngine>(false);
    if (!preview_->ok()) {
        if (error) *error = QStringLiteral("No audio device: ") + QString::fromStdString(preview_->lastError());
        return false;
    }
    stopPreviewSound();
    preview_->setMasterVolume(audioMuted_ ? 0.0f : masterVolume_);
    const AudioEngine::SoundId id = preview_->load(path);
    if (!id) {
        if (error) *error = QStringLiteral("Could not load ") + QString::fromStdString(path);
        return false;
    }
    preview_->setVolume(id, volume);
    preview_->setLooping(id, loop);
    previewId_ = id;
    if (!preview_->play(id)) {
        if (error) *error = QStringLiteral("The sound could not start.");
        return false;
    }
    return true;
}

void EngineSession::stopPreviewSound() {
    if (preview_ && previewId_) preview_->stop(previewId_);
    previewId_ = AudioEngine::SoundId{};
}

bool EngineSession::previewSoundPlaying() const {
    return preview_ && previewId_ && preview_->isPlaying(previewId_);
}

void EngineSession::setMasterVolume(float v) {
    masterVolume_ = std::clamp(v, 0.0f, 1.0f);
    applyAudioMute();
}

std::vector<ukiyo::ParticleInstance> EngineSession::particleSamples() const {
    std::vector<ukiyo::ParticleInstance> out;
    if (world_) world_->vfx.instances(&out);
    return out;
}

bool EngineSession::checkScript(const std::string& path, QString* error) const {
    ScriptHost host;
    std::string msg;
    if (host.compileFile(path, &msg)) return true;
    if (error) *error = QString::fromStdString(msg);
    return false;
}

bool EngineSession::checkScriptText(const std::string& path, const std::string& text, QString* error) const {
    ScriptHost host;
    // The search paths are the ones Play uses, so a require() in the file resolves the same way here.
    host.setSearchPaths({QDir(projectRoot_).filePath(QStringLiteral("Scripts")).toStdString(),
                         QDir(projectRoot_).filePath(QStringLiteral("assets")).toStdString()});
    std::string msg;
    if (host.compileString(text, path, &msg)) return true;
    if (error) *error = QString::fromStdString(msg);
    return false;
}

bool EngineSession::writeScriptSupportFiles(std::vector<std::string>* written, QString* error) const {
    std::string msg;
    if (!script_project::writeSupportFiles(projectRoot_.toStdString(), written, &msg)) {
        if (error) *error = QString::fromStdString(msg);
        return false;
    }
    return true;
}

const std::vector<ukiyo::ScriptInfo>& EngineSession::playScripts() const {
    static const std::vector<ukiyo::ScriptInfo> none;
    return world_ ? world_->scripts.scripts() : none;
}

bool EngineSession::reloadScripts(QString* error) {
    if (!world_) return false;
    bool ok = true;
    for (size_t i = 0; i < instances_.size(); ++i) {
        const SystemsComponents& c = instances_[i].components;
        if (!c.script.enabled || c.script.path.empty()) continue;
        std::string err;
        // reload() keeps the script's turn in the order and runs its onStart again with the new code.
        if (!world_->scripts.reload(entityName(i), &err)) {
            playWarnings_.push_back("script: " + err);
            if (ok && error) *error = QString::fromStdString(err);
            ok = false;
            continue;
        }
        if (ScriptHost* host = world_->scripts.find(entityName(i)); host && host->hasFailed())
            playWarnings_.push_back("script: " + entityName(i) + ": " + host->lastError());
        // The reloaded chunk has to hear that the scene is already running, or onSceneLoaded never runs again.
        if (ScriptHost* host = world_->scripts.find(entityName(i))) host->callSceneLoaded();
    }
    return ok;
}

bool EngineSession::startPlay(QString* error) {
    stopPlay();
    if (instances_.empty()) {
        if (error) *error = QStringLiteral("The scene has no instances to play.");
        return false;
    }
    bool anyAgent = false;
    for (const Instance& in : instances_)
        if (in.components.agent.enabled) anyAgent = true;
    if (anyAgent && !navBaked_ && !bakeNavMesh(error, nullptr)) return false;

    auto world = std::make_unique<World>(false);   // silent when the machine has no audio device
    world->audio.setMasterVolume(audioMuted_ ? 0.0f : masterVolume_);
    world->setProfiling(profiling_);
    world->setProjectRoot(projectRoot_.toStdString());
    // The project's language, so ukiyo.tr in a script returns the same text the player would show.
    if (!projectRoot_.isEmpty()) {
        LocaleCatalog catalog;
        std::string locErr;
        const std::string base = ukiyo::baseLocale(projectRoot_.toStdString());
        if (ukiyo::loadLocale(projectRoot_.toStdString(), base, &catalog, &locErr)) {
            world->setLocale(std::move(catalog));
        } else if (!locErr.empty()) {
            playWarnings_.push_back(locErr);
        }
    }
    if (anyAgent) world->nav.build(navVerts_, navTris_);
    playWarnings_.clear();

    // Instances that are not dynamic bodies are solid for the bodies that are. Their geometry becomes a
    // static collider, so a falling body lands on a floor instead of going through it.
    {
        std::vector<glm::vec3> solidVerts;
        std::vector<uint32_t> solidTris;
        for (Instance& in : instances_) {
            if (in.components.body.enabled && in.components.body.dynamic) continue;
            updateModel(*in.model, 0.0f, glm::vec3(0.0f));
            worldTriangles(*in.model, &solidVerts, &solidTris);
        }
        if (!solidTris.empty()) world->physics.addStaticMesh(solidVerts, solidTris);
    }
    // Terrain (item 18): each chunk is a static mesh collider, placed at the tile origin.
    for (Instance& in : instances_) {
        if (!in.terrainGpu) continue;
        const ukiyo::Heightmap& map = in.terrainGpu->map;
        const uint32_t n = ukiyo::chunksPerSide(map, ukiyo::kTerrainChunkCells);
        for (uint32_t cz = 0; cz < n; ++cz) {
            for (uint32_t cx = 0; cx < n; ++cx) {
                ukiyo::ChunkMesh m = ukiyo::buildChunk(map, cx, cz, ukiyo::kTerrainChunkCells);
                for (glm::vec3& p : m.positions) p += in.position;
                world->physics.addStaticMesh(m.positions, m.indices);
            }
        }
    }

    playStart_.clear();
    playLast_.clear();
    playSpeed_.clear();
    playScale_.clear();
    playTiming_ = ukiyo::FrameTiming();
    const int active = active_;
    for (size_t i = 0; i < instances_.size(); ++i) {
        Instance& in = instances_[i];
        playStart_.push_back({in.position, in.yawDeg, in.pitchDeg, in.rollDeg});
        playLast_.push_back(in.position);
        playSpeed_.push_back(0.0f);
        playScale_.push_back(in.scale);
        const SystemsComponents& c = in.components;
        if (!c.any()) continue;

        const std::string name = entityName(i);
        Entity& e = world->entity(name);
        e.position = in.position;
        // The collider is turned the way the gizmo drew the instance: a ramp tilted about X or Z is a ramp to the
        // physics too, which is the whole point of letting the gizmo turn those axes.
        e.rotationDeg = glm::vec3(in.pitchDeg, in.yawDeg, in.rollDeg);
        if (c.body.enabled) {
            if (c.body.dynamic) {
                if (c.body.box) world->attachDynamicBox(name, c.body.halfExtents, c.body.mass, e.rotationDeg);
                else world->attachDynamicSphere(name, c.body.radius, c.body.mass);
            } else {
                e.body = world->physics.addStaticBox(in.position, c.body.box ? c.body.halfExtents : glm::vec3(c.body.radius),
                                                    e.rotationDeg);
            }
        }
        if (c.character.enabled) {
            // The capsule character (item 4). In play mode, WASD walks it relative to the game camera and Space jumps.
            CharacterSettings cs;
            cs.radius = c.character.radius;
            cs.height = c.character.height;
            cs.maxSlopeDeg = c.character.maxSlopeDeg;
            cs.stepHeight = c.character.stepHeight;
            cs.jumpSpeed = c.character.jumpSpeed;
            world->attachCharacter(name, cs);
            playCharacters_.push_back({static_cast<int>(i), c.character.speed});
        }
        if (c.agent.enabled) world->attachAgent(name, c.agent.maxSpeed);
        if (c.anim.enabled) {
            std::map<std::string, float> durations;
            if (lib_)
                for (const AnimClip& clip : lib_->clips()) durations[clip.name] = clip.duration;
            e.anim = animfile::buildStateMachine(c.anim, durations);
        }
        if (c.emitter.enabled) world->vfx.spawn(c.emitter.settings, in.position);
        if (c.action.enabled) {
            ukiyo::EntityAction a;
            a.on = true;
            a.pingPong = c.action.pingPong;
            a.speed = c.action.speed;
            a.axis = c.action.axis;
            a.amplitude = c.action.amplitude;
            a.ease = c.action.ease;
            a.startOffset = c.action.startOffset;
            world->setAction(name, a, in.position, in.yawDeg);
        }
        if (c.audio.enabled && c.audio.playOnStart) {
            if (c.audio.sound.empty()) {
                playWarnings_.push_back(entityName(i) + ": no sound file is set");
            } else if (AudioEngine::SoundId id = world->soundFor(c.audio.sound)) {
                world->audio.setPosition(id, in.position);
                world->audio.setSpatial(id, true);
                world->audio.setLooping(id, c.audio.loop);
                world->audio.setVolume(id, c.audio.volume);
                world->audio.play(id);
            } else {
                playWarnings_.push_back(entityName(i) + ": could not load " + c.audio.sound);
            }
        }
    }
    if (active >= 0 && active < static_cast<int>(instances_.size()))
        world->entity("listener").position = instances_[active].position;

    // One script per instance, all of them: a level with two actors runs two scripts. A script that does not compile
    // is reported and the scene keeps running, as it does for a sound that will not load; the editor's Play page
    // shows the warning, so a typo does not take the whole level down.
    {
        std::vector<std::string> roots{QDir(projectRoot_).filePath(QStringLiteral("Scripts")).toStdString(),
                                       QDir(projectRoot_).filePath(QStringLiteral("assets")).toStdString()};
        world->setScriptSearchPaths(roots);
    }
    for (size_t i = 0; i < instances_.size(); ++i) {
        const SystemsComponents& c = instances_[i].components;
        if (!c.script.enabled || c.script.path.empty()) continue;
        std::string err;
        if (!world->loadScriptFor(entityName(i), c.script.path, &err)) playWarnings_.push_back("script: " + err);
    }

    // Locomotion clips drive the active instance's model. Other instances keep their rest pose.
    world->onClipChange = [this](const std::string& entity, const std::string& clip, float fade, float startFraction) {
        if (active_ >= 0 && entity == entityName(static_cast<size_t>(active_))) playClip(clip, fade, startFraction);
    };
    world->onBlendChange = [this](const std::string& entity, const std::vector<std::string>& clips,
                                  const std::vector<float>& weights, const std::vector<ukiyo::MotionOptions>& options,
                                  float fade) {
        if (active_ >= 0 && entity == entityName(static_cast<size_t>(active_))) playBlend(clips, weights, options, fade);
    };

    // Project settings: the fixed step and solver counts go to physics, the rest to the world.
    world->physics.setFixedStep(projectSettings_.fixedTimestep);
    world->physics.setSolverSteps(projectSettings_.velocitySolverSteps, projectSettings_.positionSolverSteps);
    world->physics.setGravity(projectSettings_.gravity);
    world->setTimeScale(projectSettings_.timeScale);
    world->setInputActions(projectSettings_.inputActions);

    // onSceneLoaded runs once the world is set up. Its errors are reported with the other play warnings.
    world->notifySceneLoaded();
    if (world->scripts.anyFailed()) playWarnings_.push_back("script: " + world->scripts.lastError());

    world_ = std::move(world);
    world_->physics.setWater(water_ ? &*water_ : nullptr);
    clipBeforePlay_ = currentClip();
    return true;
}

void EngineSession::setKeyHeld(const QString& key, bool down) {
    if (world_) world_->setKeyHeld(key.toStdString(), down);
    if (key.isEmpty()) return;
    if (down) playKeys_.insert(key.toStdString());
    else playKeys_.erase(key.toStdString());
}

void EngineSession::stopPlay() {
    if (!world_) return;
    playCharacters_.clear();
    playKeys_.clear();
    world_.reset();   // audio and physics go first
    const bool restored = restoreEditorScene();
    for (size_t i = 0; i < playStart_.size() && i < instances_.size(); ++i) {
        const Placement& p = playStart_[i];
        const glm::vec3 sc = i < playScale_.size() ? playScale_[i] : glm::vec3(1.0f);
        const float scf[3] = {sc.x, sc.y, sc.z};
        const float pos[3] = {p.position.x, p.position.y, p.position.z};
        setInstancePlacement(static_cast<int>(i), pos, p.yawDeg, p.pitchDeg, p.rollDeg, scf);
    }
    playStart_.clear();
    playLast_.clear();
    playSpeed_.clear();
    playScale_.clear();
    playClip(clipBeforePlay_);
    clipBeforePlay_.clear();
    if (restored) noteScene("Back in the editor scene", true);
}

bool EngineSession::restoreEditorScene() {
    if (!editorSaved_) return false;
    instances_ = std::move(editorInstances_);
    editorInstances_.clear();
    const int n = static_cast<int>(instances_.size());
    active_ = (editorActive_ >= 0 && editorActive_ < n) ? editorActive_ : (n > 0 ? 0 : -1);
    playStart_ = std::move(editorStart_);
    editorStart_.clear();
    editorSaved_ = false;
    applyGraphMaterials();
    bindActive();
    return true;
}

bool EngineSession::switchScene(const QString& path, QString* error) {
    const QString abs = QDir::cleanPath(QDir(projectRoot_).absoluteFilePath(path));
    scene_file::SceneContents contents;
    QString err;
    if (!scene_file::readSceneFile(abs, projectRoot_, &contents, &err)) {
        if (error) *error = err;
        return false;
    }
    for (const QString& msg : contents.errors) noteScene(msg.toStdString(), false);
    if (contents.entries.empty()) {
        if (error) *error = abs + QStringLiteral(" has no models");
        return false;
    }
    // Every model is read before anything changes, so a missing file leaves the running scene alone.
    std::vector<std::unique_ptr<Model>> models;
    for (const scene_file::SceneEntry& e : contents.entries) {
        QString merr;
        std::unique_ptr<Model> m = takeModel(e.path, &merr);
        if (!m) {
            if (error) *error = merr.isEmpty() ? QStringLiteral("could not load ") + e.path : merr;
            return false;
        }
        models.push_back(std::move(m));
    }

    // The first switch keeps the editor's scene. Later switches only replace the switched one.
    if (!editorSaved_) {
        editorSaved_ = true;
        editorInstances_ = std::move(instances_);
        editorActive_ = active_;
        editorStart_ = std::move(playStart_);
    }
    instances_.clear();
    active_ = -1;
    world_.reset();
    playStart_.clear();
    playLast_.clear();
    playSpeed_.clear();
    playScale_.clear();
    playWarnings_.clear();
    navVerts_.clear();
    navTris_.clear();
    navBaked_ = false;
    previewNav_.reset();
    const std::string keepClip = clipBeforePlay_;

    QString terrErr;
    for (size_t k = 0; k < contents.entries.size(); ++k) {
        const scene_file::SceneEntry& e = contents.entries[k];
        attachModel(std::move(models[k]), e.path, e.position, e.yawDeg);
        const int index = active_;
        setInstanceLod(index, e.lodSwitches);
        if (!setInstanceTerrain(index, e.terrain, &terrErr)) noteScene("error: " + terrErr.toStdString(), false);
        if (!e.prefab.isEmpty()) {
            std::set<std::string> overrides;
            for (const QString& o : e.overrides) overrides.insert(o.toStdString());
            setInstanceLink(index, e.prefab.toStdString(), e.prefabId.toStdString(), overrides);
        }
        setInstanceName(index, e.name.toStdString());
        setInstanceComponents(index, e.components);
    }
    setActiveInstance(std::min(contents.active, static_cast<int>(instances_.size()) - 1));

    QString perr;
    if (!startPlay(&perr)) {
        // The new scene did not start. Play stops and the editor's scene comes back.
        restoreEditorScene();
        if (error) *error = perr;
        return false;
    }
    clipBeforePlay_ = keepClip;
    noteScene("Loaded scene " + abs.toStdString(), true);
    for (const std::string& w : playWarnings_) noteScene("systems: " + w, false);
    return true;
}

void EngineSession::setAudioMuted(bool muted) {
    audioMuted_ = muted;
    applyAudioMute();
}

void EngineSession::applyAudioMute() {
    if (world_) world_->audio.setMasterVolume(audioMuted_ ? 0.0f : masterVolume_);
    if (preview_) preview_->setMasterVolume(audioMuted_ ? 0.0f : masterVolume_);
}

float EngineSession::audioVolume() const {
    return world_ ? world_->audio.masterVolume() : -1.0f;
}

uint32_t EngineSession::lastTriangles() const {
    return renderer_ ? renderer_->lastTriangles() : 0;
}

uint32_t EngineSession::lastDrawCalls() const {
    return renderer_ ? renderer_->lastDrawCalls() : 0;
}

void EngineSession::drivePlayCharacters(float dt) {
    if (!world_ || playCharacters_.empty()) return;
    // Camera-relative, the same convention as the player: yaw 0 looks from +Z, so forward is -Z. The game view owns the
    // camera; its yaw is what the author is looking along.
    const float yaw = playCamYaw_;
    const glm::vec3 forward(-std::sin(yaw), 0.0f, -std::cos(yaw));
    const glm::vec3 right(std::cos(yaw), 0.0f, -std::sin(yaw));
    auto held = [this](const char* key) { return playKeys_.count(key) != 0; };
    const bool w = held("W"), s = held("S"), d = held("D"), a = held("A");
    glm::vec3 dir = forward * (w ? 1.0f : 0.0f) - forward * (s ? 1.0f : 0.0f) + right * (d ? 1.0f : 0.0f) -
                    right * (a ? 1.0f : 0.0f);
    const float len = glm::length(dir);
    const bool jump = held("Space");
    for (const PlayCharacter& pc : playCharacters_) {
        if (pc.instance < 0 || pc.instance >= static_cast<int>(instances_.size())) continue;
        const glm::vec3 v = len > 1e-4f ? dir / len * pc.speed : glm::vec3(0.0f);
        world_->setCharacterVelocity(entityName(static_cast<size_t>(pc.instance)), v, jump);
    }
    (void)dt;
}

ukiyo::FrameTiming EngineSession::playTiming() {
    return world_ ? playTiming_ : ukiyo::FrameTiming();
}

void EngineSession::setProfiling(bool on) {
    profiling_ = on;
    if (world_) world_->setProfiling(on);
}

void EngineSession::tickPlay(float dt) {
    if (!world_ || dt <= 0.0f) return;
    // Adding or removing instances during play would desynchronise the per-instance state.
    if (playSpeed_.size() != instances_.size()) {
        stopPlay();
        return;
    }
    // Locomotion reads the speed measured over the previous step.
    for (size_t i = 0; i < instances_.size(); ++i) {
        const SystemsComponents& c = instances_[i].components;
        if (c.anim.enabled) world_->setParam(entityName(i), "speed", playSpeed_[i]);
    }
    drivePlayCharacters(dt);
    world_->tick(dt);
    if (profiling_) playTiming_ = world_->lastTiming();
    // Preloads that scripts asked for. A failed one is reported and the scene keeps running.
    for (const std::string& path : world_->takePreloads()) {
        QString err;
        if (!preloadModel(QString::fromStdString(path), &err)) noteScene("preloadModel: " + err.toStdString(), false);
    }
    // A scene switch runs here, between frames. It replaces the world, so nothing below may use it.
    if (world_->sceneSwitchReady()) {
        const std::string path = world_->takeSceneRequest();
        QString err;
        if (!switchScene(QString::fromStdString(path), &err)) noteScene("loadScene " + path + ": " + err.toStdString(), true);
        return;
    }
    for (size_t i = 0; i < instances_.size(); ++i) {
        const SystemsComponents& c = instances_[i].components;
        if (!c.any()) continue;
        Entity* e = world_->findEntity(entityName(i));
        if (!e) continue;
        if (c.body.enabled || c.agent.enabled) {
            const float pos[3] = {e->position.x, e->position.y, e->position.z};
            // A body owns its rotation too: a box that lands on a tilted plank is drawn lying along it. An agent has
            // no rotation of its own, so its yaw stays as authored.
            const bool bodyOwns = c.body.enabled;
            setInstancePlacement(static_cast<int>(i), pos, bodyOwns ? e->rotationDeg.y : instances_[i].yawDeg,
                                 bodyOwns ? e->rotationDeg.x : instances_[i].pitchDeg,
                                 bodyOwns ? e->rotationDeg.z : instances_[i].rollDeg);
        }
        const glm::vec3 moved = e->position - playLast_[i];
        playSpeed_[i] = std::sqrt(moved.x * moved.x + moved.z * moved.z) / dt;
        playLast_[i] = e->position;
    }
    if (active_ >= 0 && active_ < static_cast<int>(instances_.size()))
        world_->entity("listener").position = instances_[active_].position;
}

int EngineSession::playingEntities() const {
    if (!world_) return 0;
    int n = 0;
    for (size_t i = 0; i < instances_.size(); ++i)
        if (world_->findEntity(entityName(i))) ++n;
    return n;
}

int EngineSession::liveParticles() const {
    if (!world_) return 0;
    std::vector<ParticleInstance> list;
    world_->vfx.instances(&list);
    return static_cast<int>(list.size());
}

void EngineSession::drawPlayHud() {
    // The atlas loads once, from the editor's own font. Without it the HUD still draws its bands, just no words; the
    // glyphs a frame needs are rasterised on first use, so leaving this on costs nothing per frame.
    if (!hudFontTried_) {
        hudFontTried_ = true;
        const std::string path = std::string(UKIYO_ROOT) + "/editor/res/fonts/IBMPlexSansJP-Regular.ttf";
        if (hudFont_.loadFromFile(path, 18.0f, 1024)) {
            hudFont_.preloadAscii();
            hudCanvas_.setFont(&hudFont_);
            if (renderer_) renderer_->setHudAtlas(&hudFont_);
        }
    }
}

void EngineSession::drawParticleOverlay(const OrbitCamera& cam) {
    if (!world_ || !device_) return;
    std::vector<ParticleInstance> list;
    world_->vfx.instances(&list);
    if (list.empty()) return;

    const uint32_t w = device_->backbufferWidth();
    const uint32_t h = device_->backbufferHeight();
    if (!particles_) particles_ = std::make_unique<ParticleRenderer>(*device_, rhi::Format::Backbuffer, false);
    if (!whiteTex_) {
        rhi::TextureDesc td;
        const uint8_t white[4] = {255, 255, 255, 255};
        whiteTex_ = device_->createTexture(td, white);
    }

    // The overlay goes in after tone mapping, so particles are not exposed or graded. They are
    // drawn without depth testing, so they can show through geometry in front of them.
    FrameUBO f{};
    f.view = cam.view();
    f.proj = cam.proj(float(w) / float(h));
    f.viewProj = f.proj * f.view;
    f.invViewProj = glm::inverse(f.viewProj);
    f.camPos = glm::vec4(cam.eye(), 1.0f);
    f.screenParams = glm::vec4(float(w), float(h), 0.0f, 0.0f);
    for (auto& m : f.shadowMatrix) m = glm::mat4(1.0f);
    auto frameUbo = device_->createBuffer(rhi::BufferUsage::Uniform, sizeof(FrameUBO));
    frameUbo->upload(&f, sizeof(f));
    auto frameSet = device_->createDescriptorSet(rhi::SetType::Frame);
    frameSet->setBuffer(0, frameUbo.get());
    frameSet->setTexture(1, whiteTex_.get());

    rhi::PassDesc pass;
    pass.backbuffer = true;
    pass.clearColor = false;
    pass.clearDepth = false;
    device_->beginPass(pass);
    device_->setViewport(0, 0, static_cast<int>(w), static_cast<int>(h));
    particles_->draw(*device_, *frameSet, list);
    device_->endPass();
}

} // namespace editor
