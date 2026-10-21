// The .ukscene reader. The editor's Open Scene and the runtime scene switch both read a scene through
// here, so an instance means the same thing on either path. Nothing here touches the editor window.
#pragma once

#include <optional>
#include <vector>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#include "engine/terrain.h"
#include "engine/lights.h"
#include "engine/water.h"
#include "systems/components.h"

namespace editor::scene_file {

// One instance as the scene file describes it, with paths resolved to absolute.
struct SceneEntry {
    QString path;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float yawDeg = 0.0f;
    QString prefab;                 // absolute .ukprefab path, or empty for a plain model
    QString prefabId;
    QStringList overrides;
    ukiyo::SystemsComponents components;
    QString name;                   // Hierarchy name, or empty for the file name
    std::vector<float> lodSwitches; // mesh LOD switch distances (item 15), empty for LOD off
    std::optional<ukiyo::TerrainSettings> terrain;   // terrain tile (item 18); the instance position is its origin
};

// The instances of a scene. A problem with one instance is listed in errors and that instance is
// skipped. The rest still load.
struct SceneContents {
    std::vector<SceneEntry> entries;
    QStringList errors;
    int active = 0;                 // the saved active instance, not yet checked against entries
    bool hasWater = false;          // the scene has a \"water\" object (item 16)
    ukiyo::WaterVolume water;
};

// Reads the instances of a parsed .ukscene. Version 2 lists them under "models". Version 1, and any file
// without "models", has one model under "model".
void parseSceneEntries(const QJsonObject& root, const QString& projectRoot, SceneContents* out);

// Reads and parses a .ukscene file. Returns false with *err set when the file cannot be read or is not a
// Ukiyo scene. Problems with single instances go to out->errors.
bool readSceneFile(const QString& path, const QString& projectRoot, SceneContents* out, QString* err);

// The placeholder model for an empty object. It is written to the temp folder on first use.
QString emptyModelPath();
bool ensureEmptyModelFile(QString* err);

// Reads n numbers from a JSON array into out. Missing values keep what out already holds.
void readFloats(const QJsonValue& v, float* out, int n);

// The optional \"water\" object of a scene (item 16). *present is false when the scene has none. Values are clamped
// to the ranges the renderer accepts, so a hand-edited file cannot break the surface.
void parseWater(const QJsonObject& root, ukiyo::WaterVolume* out, bool* present);
QJsonObject waterToJson(const ukiyo::WaterVolume& v);

// The optional \"lights\" array of a scene (item 6): authored point and spot lights, plus at most one directional key
// light. Values are clamped to what the renderer accepts, so a hand-edited file cannot produce a light that shades
// nothing or one that floods the frame. A scene with no \"lights\" keeps the engine's two stylistic fills.
std::vector<ukiyo::SceneLight> readLights(const QJsonValue& v);
QJsonArray lightsToJson(const std::vector<ukiyo::SceneLight>& lights);

// The optional \"lod\" array of an instance (item 15): switch distances in metres. Bad values are dropped.
std::vector<float> readLodSwitches(const QJsonValue& v);
QJsonArray lodSwitchesToJson(const std::vector<float>& switches);

// The optional \"terrain\" object of an instance (item 18). The heightmap path is stored relative to the project.
std::optional<ukiyo::TerrainSettings> readTerrain(const QJsonValue& v, const QString& projectRoot);
QJsonObject terrainToJson(const ukiyo::TerrainSettings& t, const QString& projectRoot);

} // namespace editor::scene_file
