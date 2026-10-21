// Bridge between the editor and the Ukiyo engine. Owns the render device, the renderer, the
// loaded model and its animation. Frames are rendered offscreen and returned as QImage. Every
// method runs on the RenderWorker thread; the GUI thread never calls into this class directly.
#pragma once

#include <atomic>
#include <memory>
#include <set>
#include <optional>

#include "systems/audio.h"
#include "systems/navigation.h"
#include "systems/vfx.h"

#include <map>
#include <set>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <QImage>
#include <QString>

#include "engine/anim.h"
#include "engine/hud.h"
#include "engine/text_atlas.h"
#include "engine/camera.h"
#include "engine/model.h"
#include "engine/project_settings.h"
#include "engine/renderer.h"
#include "engine/terrain_edit.h"
#include "engine/terrain_gpu.h"
#include "engine/water.h"
#include "engine/graph_material.h"
#include "engine/particle_renderer.h"
#include "rhi/rhi.h"
#include "systems/components.h"
#include "systems/world.h"

using namespace ukiyo;

namespace editor {

// Terrain editing (item 18, steps 4 and 5). The tool in use for the viewport brush.
enum class TerrainTool : uint8_t { None, Noise, Terrace, Stamp, LayerMask, Density };

// The brush as the Inspector sets it. Heights move by `amplitude` (metres) at full weight for the noise and stamp
// tools, and are quantised to `steps` levels for the terrace. The mask tools paint their channel towards 255 when
// `add` is set, and towards 0 otherwise.
struct TerrainBrushSettings {
    TerrainTool tool = TerrainTool::None;
    float radius = 8.0f;        // metres
    float falloff = 0.5f;       // 0 soft to 1 hard edge
    float strength = 0.3f;      // per dab, 0 to 1; a negative value reverses a height brush
    float amplitude = 0.5f;     // metres at full weight
    float wavelength = 40.0f;   // metres per noise feature
    uint32_t steps = 6;         // terrace levels
    int channel = 0;            // layer (0 to 7) for LayerMask, foliage type for Density
    bool add = true;
    QString stampFile;          // empty: the built-in dome
};

// One undoable change to a tile. `target` is 0 for heights, 1 for a layer mask and 2 for a density mask. The `before`
// values undo it and the `after` values redo it. `createdMask` means the change made the mask, so undo removes it.
struct TerrainEdit {
    QString path;               // heightmap file: the key of the in-memory tile
    int target = 0;
    int channel = 0;
    bool createdMask = false;
    std::vector<ukiyo::HeightEdit> heights;
    std::vector<ukiyo::PaintEdit> paint;
};

class EngineSession {
public:
    // Initial offscreen size. The views request their own size once they are laid out.
    static constexpr uint32_t kWidth = 960;
    static constexpr uint32_t kHeight = 540;

    // Creates the headless Vulkan device and renderer. Returns false with `error` set on failure.
    bool init(QString* error);

    // Loads a VRM or glTF model into the active instance, replacing its model. With no instance yet,
    // it becomes the first one. Starts the idle loop.
    bool loadModel(const QString& path, QString* error);
    // Applied to models loaded from now on (RealToon smooth object normals).
    void setSmoothNormals(bool on) { smoothNormals_ = on; }
    // Alpha-coverage-preserving mips (GH-104289) and the cutoff they preserve. Load-time, like smooth normals.
    void setPreserveAlphaCoverage(bool on) { preserveAlphaCoverage_ = on; }
    void setAlphaTestThreshold(float t) { alphaTestThreshold_ = t; }

    // Adds a model to the scene at `position` (metres) with `yawDeg` (degrees about +Y). The new
    // instance becomes active; its index is returned in *index.
    bool addModel(const QString& path, const float position[3], float yawDeg, QString* error, int* index);
    // Project folder. Scene switches resolve their paths against it.
    void setProjectRoot(const QString& root) { projectRoot_ = root; }
    // The scene's water volume (item 16), or none. Drawn by render() until it is changed.
    // It also reaches the play world's physics for buoyancy (item 17). The wave clock is the render time.
    void setWater(const ukiyo::WaterVolume* w) {
        if (w) water_ = *w;
        else water_.reset();
        if (world_) world_->physics.setWater(w);
    }
    // Reads a model ahead of time, so an instance that uses it is added without a read. A staged model
    // is used once, and at most kMaxStaged are kept. A file that changed since staging is read again.
    bool preloadModel(const QString& path, QString* error);
    static constexpr size_t kMaxStaged = 8;
    // Messages for the editor's log. `structural` means the scene's instances changed, so the
    // Hierarchy and Inspector should be rebuilt from the worker.
    struct SceneNote {
        std::string text;
        bool structural = false;
    };
    bool hasSceneNotes() const { return !sceneNotes_.empty(); }
    std::vector<SceneNote> takeSceneNotes();
    // True while a scene loaded by a script runs in place of the editor's scene. Saving is refused then.
    bool sceneSwitched() const { return editorSaved_; }
    // Makes instance `index` the active one (the target of the node and graph queries).
    bool setActiveInstance(int index);
    // Moves and turns an instance. Takes effect on the next frame.
    bool setInstancePlacement(int index, const float position[3], float yawDeg);

    // Snapshot of one instance, for saving the scene.
    struct PlacedModel {
        std::string path;
        float position[3] = {0.0f, 0.0f, 0.0f};
        float yawDeg = 0.0f;
        std::string prefab;                 // .ukprefab this instance is linked to, or empty
        std::string prefabId;
        std::set<std::string> overrides;    // keys that the instance sets itself: "translation", "yawDeg"
        std::string name;                   // Hierarchy name chosen by the user, or empty for the file name
        std::vector<float> lodSwitches;     // mesh LOD switch distances in metres, empty for LOD off
        std::optional<ukiyo::TerrainSettings> terrain;   // terrain tile settings, or none
    };
    int instanceCount() const { return static_cast<int>(instances_.size()); }
    int activeInstance() const { return active_; }
    // Removes every instance. Used by New Scene and Open Scene before they load.
    void clearInstances();
    // Links an instance to a prefab and records which keys it overrides. The editor owns the meaning.
    bool setInstanceLink(int index, const std::string& prefab, const std::string& prefabId,
                         const std::set<std::string>& overrides);
    // Sets the name shown in the Hierarchy. Empty restores the file name.
    bool setInstanceName(int index, const std::string& name);
    // Sets the LOD switch distances of one object and builds its levels (item 15). Values are cleaned first.
    // Empty turns LOD off. Returns false with *error set when the object or the device is missing.
    bool setInstanceLod(int index, const std::vector<float>& switches, QString* error = nullptr);
    // Sets or clears the terrain of one object (item 18). Loads the heightmap and uploads its chunks. A failure leaves
    // the object as it was.
    bool setInstanceTerrain(int index, const std::optional<ukiyo::TerrainSettings>& terrain, QString* error = nullptr);

    // Terrain editing (item 18, steps 4 and 5). Edits go to the in-memory tile, keyed by heightmap path. The GPU copy
    // follows them now, or at flushTerrainUploads when `upload` is false (during a stroke).
    int pickTerrain(glm::vec3 origin, glm::vec3 dir, glm::vec3* worldHit) const;   // nearest tile hit, or -1
    bool terrainDab(int index, glm::vec3 worldHit, const TerrainBrushSettings& b, TerrainEdit* out, QString* error);
    bool terrainErode(int index, const ukiyo::ErosionSettings& s, TerrainEdit* out, QString* error);
    bool applyTerrainEdit(const TerrainEdit& e, bool useAfter, bool upload, QString* error);
    void flushTerrainUploads();
    // Writes the edited heightmaps and masks beside their files, and points the tiles at the mask files.
    bool saveTerrainEdits(QString* error);
    void clearTerrainEdits();
    std::vector<float> terrainHeights(int index) const;
    int terrainFoliageCount(int index) const;
    float terrainHeightAt(int index, float x, float z) const;
    PlacedModel instanceInfo(int index) const;

    // Loads the animation clip library used by the timeline and idle loop.
    bool loadAnimLibrary(const QString& path, QString* error);
    // Keyframe and event edits from the .ukanim file, applied on top of the library's clips.
    void setClipEdits(const ukiyo::ClipEdits& edits);
    // The rig and the unedited clips, for the Animation editor.
    void libraryData(ukiyo::SourceRig* rig, std::vector<ukiyo::AnimClip>* clips) const;

    bool hasModel() const { return activeModel() != nullptr; }
    Model* model() { return activeModel(); }
    std::string modelPath() const;

    // Clip names of the loaded library, for the timeline clip picker.
    std::vector<std::string> clipNames() const;
    float clipDuration(const std::string& name) const;

    // Plays a clip (empty name = back to idle).
    void playClip(const std::string& name, float fade = 0.3f, float startFraction = 0.0f);
    // A blend-tree state: fade >= 0 starts the mix, fade < 0 only changes the weights of the mix playing.
    void playBlend(const std::vector<std::string>& clips, const std::vector<float>& weights,
                   const std::vector<ukiyo::MotionOptions>& options, float fade);
    const std::string& currentClip() const;

    // Resizes the offscreen target so the next frame is exactly w x h. Frames then fill the view.
    void setRenderSize(uint32_t w, uint32_t h);

    // Advances the animation to `time` (dt is the elapsed step, 0 when paused) and renders a
    // frame with `cam` and `settings`.
    QImage render(const OrbitCamera& cam, float time, float dt, const RenderSettings& settings);

    // Game view Mute Audio. Applies to the play-mode audio engine, including one created later.
    void setAudioMuted(bool muted);
    bool audioMuted() const { return audioMuted_; }
    // Master volume of the play-mode audio engine, or -1 when not playing.
    float audioVolume() const;
    // Mesh draw calls issued by the last rendered frame.
    uint32_t lastDrawCalls() const;
    uint32_t lastTriangles() const;

    // Model-space node world position (for framing the camera).
    glm::vec3 nodeWorldPosition(int node) const;

    // Assigns a .ukmat (absolute path; empty clears) to every mesh primitive on a node. Returns an
    // error message, or an empty string on success.
    QString setNodeGraphMaterial(int node, const QString& ukmatPath);
    QString setNodeMmdToon(int node, bool on);
    // Loads a colour grading LUT from an absolute PNG path; empty restores the identity LUT.
    QString setGradingLut(const QString& absPath);
    // Re-reads changed .ukmat and .ukshader files and re-applies every node assignment.
    QString refreshGraphMaterials();

    // --- Game systems ------------------------------------------------------------------------
    // Settings for one instance (physics, agent, audio, animation, script, particles). Paths in
    // SystemsComponents are absolute here; the scene file stores them project-relative.
    void setInstanceComponents(int index, const SystemsComponents& c);
    SystemsComponents instanceComponents(int index) const;
    // Collects the world-space geometry of every instance into the navigation input and checks that
    // a navmesh can be built from it. *triangles receives the count.
    // progress, when given, counts the bake stages (NavMesh::kBakeStages) so another thread can show them.
    bool bakeNavMesh(QString* error, int* triangles, std::atomic<int>* progress = nullptr);
    // A straight path on the baked navmesh, for the Inspector's path preview. Fails with a reason in error.
    bool previewPath(const glm::vec3& from, const glm::vec3& to, std::vector<glm::vec3>* path, QString* error);
    // Plays a sound file once through the editor's own audio device, outside play mode.
    bool previewSound(const std::string& path, float volume, bool loop, QString* error);
    void stopPreviewSound();
    bool previewSoundPlaying() const;
    // The audio master volume (0 to 1). Mute still silences everything.
    void setMasterVolume(float v);
    float masterVolume() const { return masterVolume_; }
    // Live particles of the running play session, for the emitter preview.
    std::vector<ukiyo::ParticleInstance> particleSamples() const;
    // Compiles a Lua script without running it. Returns false with the Lua message, which includes the line.
    bool checkScript(const std::string& path, QString* error) const;
    bool navMeshBaked() const { return navBaked_; }
    // Starts play mode: builds a World from the components. Entities are named "instance<N>", and the
    // listener follows the active instance. Fails with a message if the setup cannot run.
    bool startPlay(QString* error);
    // Project settings (time, physics, input). They apply when play starts, and a key change
    // during play goes straight to the running world. Key names are portable key text.
    void setProjectSettings(const engine::ProjectSettings& settings) { projectSettings_ = settings; }
    void setKeyHeld(const QString& key, bool down);
    // Stops play mode and puts every instance back where it was before play.
    void stopPlay();
    bool playing() const { return world_ != nullptr; }
    // The world that exists while playing, for the parts of the editor that drive its input directly (the rebinding
    // screen). Only valid when playing(); the key path goes through setKeyHeld.
    ukiyo::World& world() { return *world_; }
    // Non-fatal problems found while starting play, such as a sound file that does not load.
    const std::vector<std::string>& playWarnings() const { return playWarnings_; }
    // Advances play mode by dt seconds and moves instances to their simulated positions. Render calls
    // this with the frame step, so it does nothing while paused.
    void tickPlay(float dt);
    int playingEntities() const;
    // Snapshot of the particles in play (count only, for the Inspector).
    int liveParticles() const;

    // Releases the model, animation, renderer and device, in that order, on the calling thread.
    void shutdown();

private:
    bool smoothNormals_ = false;
    bool preserveAlphaCoverage_ = true;
    float alphaTestThreshold_ = 0.5f;
    std::unique_ptr<rhi::Device> device_;
    std::unique_ptr<Renderer> renderer_;
    std::optional<ukiyo::WaterVolume> water_;
    bool audioMuted_ = false;
    void applyAudioMute();
    // Scene instances. Each owns its model; all of them are rendered. The active one is animated
    // by player_ and is the target of the node and graph queries.
    struct Instance {
        std::unique_ptr<Model> model;
        std::string path;
        glm::vec3 position{0.0f};
        float yawDeg = 0.0f;
        // Graph material assignments by this model's node index.
        std::map<int, std::string> nodeGraph;
        // Nodes drawn with the MMD-style toon look.
        std::set<int> nodeMmd;
        std::string prefab;
        std::string prefabId;
        std::set<std::string> overrides;
        SystemsComponents components;
        std::string name;
        std::vector<float> lodSwitches;   // item 15: switch distances, empty for LOD off
        std::optional<ukiyo::TerrainSettings> terrain;   // item 18: the tile's settings
        std::unique_ptr<ukiyo::TerrainGpu> terrainGpu;   // item 18: uploaded chunks, null without terrain
    };
    std::vector<Instance> instances_;

    // In-memory terrain edits, keyed by heightmap path, so a tile reloaded from the scene (after an undo, say) keeps
    // them. Dirty flags say what the next save must write.
    struct TerrainEditState {
        ukiyo::Heightmap map;
        bool heightsDirty = false;
        std::vector<uint8_t> layerPaint;                        // size * size * 8, empty when no layer mask exists
        bool layerDirty = false;
        std::vector<std::vector<uint8_t>> density;              // per foliage type, empty when no density mask exists
        std::vector<uint8_t> densityDirty;
        // The grass detail map (item 19), read the same way as a foliage density map.
        std::vector<uint8_t> grassDensity;
        std::string grassDensitySource;
        // The image each mask was read from or written to. When the settings name another image, it is read again.
        std::vector<std::string> maskSource;                    // the layer mask files, as the settings hold them
        std::vector<std::string> densitySource;                 // per foliage type
    };
    TerrainEditState* ensureTerrainState(const ukiyo::TerrainSettings& ts, QString* error);
    bool buildTerrainGpu(const ukiyo::TerrainSettings& ts, const TerrainEditState& st,
                         std::unique_ptr<ukiyo::TerrainGpu>* out, QString* error);
    void refreshTerrainTiles(const std::string& path);
    std::map<std::string, TerrainEditState> terrainEdits_;
    std::set<std::string> terrainUploads_;                      // tiles whose GPU copy is behind their edits
    ukiyo::Stamp terrainStamp_;                                 // the loaded stamp image, cached by file
    std::string terrainStampFile_;
    bool terrainStampLoaded_ = false;

    // Game systems state. world_ exists only in play mode. The navigation geometry is kept so the
    // World can build its navmesh at start.
    std::unique_ptr<World> world_;
    engine::ProjectSettings projectSettings_;
    std::vector<glm::vec3> navVerts_;
    std::vector<uint32_t> navTris_;
    bool navBaked_ = false;
    std::unique_ptr<NavMesh> previewNav_;      // built on the first path preview after a bake
    float masterVolume_ = 1.0f;
    std::unique_ptr<AudioEngine> preview_;     // the sound preview's device, made on first use
    AudioEngine::SoundId previewId_{};
    struct Placement {
        glm::vec3 position{0.0f};
        float yawDeg = 0.0f;
    };
    std::vector<Placement> playStart_;   // placements before play, restored by stopPlay
    std::vector<glm::vec3> playLast_;    // entity positions after the previous tick
    std::vector<float> playSpeed_;       // horizontal speed per instance, fed to the animation machines
    // Instances with a capsule character (item 4), and the walk speed each was authored with. Driven from the game
    // view's WASD in play mode.
    struct PlayCharacter {
        int instance = -1;
        float speed = 4.0f;
    };
    std::vector<PlayCharacter> playCharacters_;
    std::set<std::string> playKeys_;   // keys held during play, in portable key text, for the character input
    float playCamYaw_ = 0.0f;          // the game view's yaw from the last render, for camera-relative walking
    std::vector<std::string> playWarnings_;
    std::string clipBeforePlay_;
    std::unique_ptr<ParticleRenderer> particles_;
    void drawParticleOverlay(const OrbitCamera& cam);
    // Walks the capsule characters from the game view's WASD during play (item 4).
    void drivePlayCharacters(float dt);
    // The HUD over play mode: title, frame statistics and the first play warning, from the same layout the player uses.
    void drawPlayHud();
    void setHudVisible(bool on) { hudVisible_ = on; }
    bool hudVisible() const { return hudVisible_; }
    FontAtlas hudFont_;
    HudCanvas hudCanvas_;
    bool hudFontTried_ = false;
    bool hudVisible_ = true;
    int active_ = -1;
    Model* activeModel() const;

    // Scene switching during play. The editor's scene is kept while a switched scene runs, so Stop can
    // put it back. Only the first switch keeps it.
    QString projectRoot_;
    struct StagedModel {
        std::unique_ptr<Model> model;
        qint64 modified = -1;
        bool smoothNormals = false;
        bool preserveAlphaCoverage = true;
        float alphaTestThreshold = 0.5f;
    };
    std::map<std::string, StagedModel> staged_;
    std::unique_ptr<Model> takeModel(const QString& path, QString* error);
    void attachModel(std::unique_ptr<Model> m, const QString& path, const float position[3], float yawDeg);
    bool switchScene(const QString& path, QString* error);
    bool restoreEditorScene();
    bool editorSaved_ = false;
    std::vector<Instance> editorInstances_;
    int editorActive_ = -1;
    std::vector<Placement> editorStart_;
    std::vector<SceneNote> sceneNotes_;
    void noteScene(const std::string& text, bool structural);
    void bindActive();
    static glm::mat4 placementOf(const glm::vec3& position, float yawDeg);
    std::unique_ptr<AnimLibrary> lib_;
    ukiyo::ClipEdits clipEdits_;
    AnimPlayer player_;
    uint32_t frame_ = 0;

    // Shader Graph materials, cached by .ukmat path. An entry is rebuilt when either its material
    // or its shader file changes. Node assignments are kept so they can be re-applied after a
    // rebuild or a model load.
    struct GraphEntry {
        std::unique_ptr<GraphMaterial> material;
        qint64 matModified = -1;
        qint64 shaderModified = -1;
        std::string textureSig;     // binding, path and mtime of every texture file the material binds
        QString textureError;       // a texture that could not be read; reported again on cache hits
    };
    std::map<std::string, GraphEntry> graphMats_;
    std::unique_ptr<rhi::Texture> whiteTex_;

    QString applyGraphMaterials();
    GraphMaterial* graphMaterialFor(const std::string& ukmatPath, QString* error);
};

} // namespace editor
