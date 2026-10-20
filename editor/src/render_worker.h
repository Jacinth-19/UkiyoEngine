// Render thread for the editor. The engine device, renderer, model and animation live on this
// thread only. The GUI thread sends commands with queued lambdas, so they run in order, and it
// receives finished frames through the frameReady signal. Loads and queries use blocking calls
// because the GUI needs their answer before it continues.
#pragma once

#include <atomic>
#include <memory>

#include <optional>
#include <vector>

#include <glm/glm.hpp>
#include <QImage>
#include <QObject>
#include <QString>

#include "engine/camera.h"
#include "engine/renderer.h"
#include "input/rebind.h"
#include "systems/components.h"

#include <QStringList>
#include "engine_session.h"

namespace editor {

// One material slot of a mesh node: a primitive the node draws.
struct SlotInfo {
    int primitive = -1;
    QString material;          // material name from the model, or empty
    uint32_t triangles = 0;
    bool doubleSided = false;
    bool blend = false;
    bool mask = false;
};

// Rest-pose snapshot of one node, so the GUI never touches the engine model directly.
struct NodeInfo {
    QString name;
    std::vector<SlotInfo> materialSlots;   // one per primitive this node draws, in draw order
    int parent = -1;
    bool hasMatrix = false;
    float translation[3] = {0.0f, 0.0f, 0.0f};
    float rotationDeg[3] = {0.0f, 0.0f, 0.0f};
    float scale[3] = {1.0f, 1.0f, 1.0f};
    bool hasMesh = false;          // at least one primitive is drawn by this node
    QString graphMaterial;         // project-relative .ukmat assigned in the Inspector, or empty
    bool mmdToon = false;          // MMD-style toon look on this node's meshes (Inspector checkbox)
};

struct ModelInfo {
    std::vector<NodeInfo> nodes;
    int headNode = -1;
    float headY = 1.45f;
};

struct ClipInfo {
    QString name;    // empty for the idle loop
    QString label;   // text shown in the clip picker
    float duration = 0.0f;
};

// One model in the scene, for saving and for the Hierarchy.
struct InstanceInfo {
    QString path;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float yawDeg = 0.0f;
    QString prefab;             // linked .ukprefab, empty for a plain model
    QString prefabId;
    QStringList overrides;      // "translation", "yawDeg"
    QString name;               // Hierarchy name, or empty for the file name
    ukiyo::SystemsComponents components;   // physics, agent, audio, animation, script, particles
    std::vector<float> lodSwitches;        // mesh LOD switch distances, empty for LOD off
    std::optional<ukiyo::TerrainSettings> terrain;   // terrain tile settings, or none
};

struct RenderRequest {
    ukiyo::OrbitCamera cam;
    ukiyo::RenderSettings settings;
    float time = 0.0f;
    float dt = 0.0f;
    bool game = false;   // true for the Game view, false for the Scene view
    uint32_t width = 0;  // requested frame size; 0 keeps the current size
    uint32_t height = 0;
};

class RenderWorker : public QObject {
    Q_OBJECT
public:
    explicit RenderWorker(QObject* parent = nullptr);

    // Worker-thread operations. The GUI calls them through MainWindow::runOnWorker (blocking)
    // or postToWorker (queued).
    bool init(QString* error);
    bool loadLibrary(const QString& path, QString* error, std::vector<ClipInfo>* clips);
    bool loadModel(const QString& path, QString* error, ModelInfo* info);
    void setSmoothNormals(bool on) { session_.setSmoothNormals(on); }
    void setPreserveAlphaCoverage(bool on) { session_.setPreserveAlphaCoverage(on); }
    void setAlphaTestThreshold(float t) { session_.setAlphaTestThreshold(t); }
    void setWater(bool on, const ukiyo::WaterVolume& w) { session_.setWater(on ? &w : nullptr); }
    // Adds a model to the scene and makes it active. *info describes the new active model.
    bool addModel(const QString& path, const float position[3], float yawDeg, QString* error, ModelInfo* info,
                  int* index);
    // Makes an instance active and describes it in *info.
    bool activateInstance(int index, ModelInfo* info);
    bool setInstancePlacement(int index, const float position[3], float yawDeg);
    bool setInstanceLink(int index, const QString& prefab, const QString& prefabId, const QStringList& overrides);
    // Removes every model from the scene.
    void clearScene() { session_.clearInstances(); }
    // Draws one frame synchronously, every instance included. For tests and captures.
    QImage renderNow(const ukiyo::OrbitCamera& cam) {
        return session_.render(cam, 0.0f, 0.0f, ukiyo::RenderSettings{});
    }
    std::vector<InstanceInfo> instances() const;
    // Game systems. Play mode runs on this thread too, one tick per rendered frame.
    void setInstanceComponents(int index, const ukiyo::SystemsComponents& c) { session_.setInstanceComponents(index, c); }
    void setCharacterFx(const ukiyo::CharacterFx& fx) { if (auto* m = session_.model()) ukiyo::setCharacterFx(*m, fx); }
    int setHairFace(const glm::vec3& anchor, float radius) {
        auto* m = session_.model();
        return m ? ukiyo::setHairFace(*m, anchor, radius) : 0;
    }
    void setOccludable(bool on) { if (auto* m = session_.model()) ukiyo::setOccludable(*m, on); }
    void setAudioMuted(bool muted) { session_.setAudioMuted(muted); }
    bool audioMuted() const { return session_.audioMuted(); }
    float audioVolume() const { return session_.audioVolume(); }
    bool setInstanceName(int index, const QString& name) { return session_.setInstanceName(index, name.toStdString()); }
    bool setInstanceLod(int index, const std::vector<float>& switches, QString* error = nullptr) {
        return session_.setInstanceLod(index, switches, error);
    }
    bool setInstanceTerrain(int index, const std::optional<ukiyo::TerrainSettings>& t, QString* error = nullptr) {
        return session_.setInstanceTerrain(index, t, error);
    }
    // Terrain editing (item 18, steps 4 and 5).
    int pickTerrain(glm::vec3 origin, glm::vec3 dir, glm::vec3* worldHit) const {
        return session_.pickTerrain(origin, dir, worldHit);
    }
    bool terrainDab(int index, glm::vec3 hit, const TerrainBrushSettings& b, TerrainEdit* out, QString* error) {
        return session_.terrainDab(index, hit, b, out, error);
    }
    bool terrainErode(int index, const ukiyo::ErosionSettings& s, TerrainEdit* out, QString* error) {
        return session_.terrainErode(index, s, out, error);
    }
    bool applyTerrainEdit(const TerrainEdit& e, bool useAfter, bool upload, QString* error) {
        return session_.applyTerrainEdit(e, useAfter, upload, error);
    }
    void flushTerrainUploads() { session_.flushTerrainUploads(); }
    bool saveTerrainEdits(QString* error) { return session_.saveTerrainEdits(error); }
    void clearTerrainEdits() { session_.clearTerrainEdits(); }
    std::vector<float> terrainHeights(int index) const { return session_.terrainHeights(index); }
    int terrainFoliageCount(int index) const { return session_.terrainFoliageCount(index); }
    float terrainHeightAt(int index, float x, float z) const { return session_.terrainHeightAt(index, x, z); }
    ukiyo::SystemsComponents instanceComponents(int index) const { return session_.instanceComponents(index); }
    bool bakeNavMesh(QString* error, int* triangles, std::atomic<int>* progress = nullptr) {
        return session_.bakeNavMesh(error, triangles, progress);
    }
    bool previewPath(const glm::vec3& from, const glm::vec3& to, std::vector<glm::vec3>* path, QString* error) {
        return session_.previewPath(from, to, path, error);
    }
    bool previewSound(const QString& path, float volume, bool loop, QString* error) {
        return session_.previewSound(path.toStdString(), volume, loop, error);
    }
    void stopPreviewSound() { session_.stopPreviewSound(); }
    bool previewSoundPlaying() const { return session_.previewSoundPlaying(); }
    void setMasterVolume(float v) { session_.setMasterVolume(v); }
    float masterVolume() const { return session_.masterVolume(); }
    std::vector<ukiyo::ParticleInstance> particleSamples() const { return session_.particleSamples(); }
    bool checkScript(const QString& path, QString* error) const { return session_.checkScript(path.toStdString(), error); }
    bool navMeshBaked() const { return session_.navMeshBaked(); }
    bool startPlay(QString* error) {
        const bool ok = session_.startPlay(error);
        reportScene();
        return ok;
    }
    void stopPlay() {
        session_.stopPlay();
        reportScene();
    }
    // Scene switches during play. Notes go to the GUI through sceneChanged.
    void setProjectRoot(const QString& root) { session_.setProjectRoot(root); }
    std::vector<EngineSession::SceneNote> takeSceneNotes() { return session_.takeSceneNotes(); }
    bool sceneSwitched() const { return session_.sceneSwitched(); }
    bool preloadModel(const QString& path, QString* error) { return session_.preloadModel(path, error); }
    void setProjectSettings(const engine::ProjectSettings& s) { session_.setProjectSettings(s); }
    void setKeyHeld(const QString& key, bool down) { session_.setKeyHeld(key, down); }
    // Starts a capture on the live world's input state, so a rebinding screen rebinds the binding the running game is
    // actually reading. Empty on success, otherwise why it could not start.
    QString beginRebind(const QString& action);
    // Feeds one key press to the waiting capture and answers with the control path it produced ("<Keyboard>/space"),
    // empty when nothing was captured. The press and its release are delivered together, so a key that was only
    // pressed to bind it does not stay held in the game. This is the editor's input path: play mode gets its keys from
    // the Game view's key events (setKeyHeld), not from a poller, so a page that listens for one has to deliver it the
    // same way.
    QString rebindKey(const QString& key);
    // Stops the capture where it is. The binding is left alone: a capture that has not applied anything has nothing
    // to undo, and one that has is the page's to write down (it holds the same result).
    void cancelRebind();
    bool playing() const { return session_.playing(); }
    std::vector<std::string> playWarnings() const { return session_.playWarnings(); }
    int liveParticles() const { return session_.liveParticles(); }
    void playTick(float dt) {
        session_.tickPlay(dt);
        reportScene();
    }
    int activeInstance() const { return session_.activeInstance(); }
    void playClip(const QString& name);
    // Clip edits for the loaded library, and the unedited rig and clips for the Animation editor.
    void setClipEdits(const ukiyo::ClipEdits& edits);
    void libraryData(ukiyo::SourceRig* rig, std::vector<ukiyo::AnimClip>* clips) const;
    void setRestPose(int node, const float* t, const float* rotDeg, const float* s);
    QString setNodeGraphMaterial(int node, const QString& ukmatPath);
    QString setNodeMmdToon(int node, bool on);
    QString setGradingLut(const QString& absPath);
    QString refreshGraphMaterials();
    glm::vec3 nodeWorldPosition(int node) const;
    void renderFrame(const RenderRequest& req);
    void shutdown();

signals:
    void frameReady(QImage image, bool game, float clipLength, double renderMs, uint32_t drawCalls, uint32_t triangles);
    // Scene notes are waiting. The GUI takes them with takeSceneNotes().
    void sceneChanged();

private:
    // Fills *info from the active model. Renders once, so the world transforms are current.
    void describeActive(ModelInfo* info);
    // Emits sceneChanged when the session has notes.
    void reportScene();
    EngineSession session_;
    // The capture, on the worker thread, on the running world (see input/rebind.h). Null when nothing is waiting.
    std::unique_ptr<ukiyo::input::RebindOperation> rebindOp_;
    std::string rebindCaptured_;
};

} // namespace editor
