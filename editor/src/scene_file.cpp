#include "scene_file.h"

#include <algorithm>
#include <optional>
#include <initializer_list>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>

#include "prefab_file.h"
#include "systems_json.h"

#include "engine/cull.h"

namespace editor::scene_file {

// An empty object is a real one-node glTF file with no meshes. It loads, duplicates and undoes like a model.
QString emptyModelPath() {
    return QDir::tempPath() + QStringLiteral("/ukiyo_editor/empty_object.glb");
}

bool ensureEmptyModelFile(QString* err) {
    const QString path = emptyModelPath();
    const QFileInfo fi(path);
    if (fi.exists() && fi.size() > 0) return true;
    QDir().mkpath(fi.absolutePath());
    QByteArray json = QByteArrayLiteral(
        "{\"asset\":{\"version\":\"2.0\",\"generator\":\"Ukiyo editor\"},\"scene\":0,"
        "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"name\":\"Empty Object\"}]}");
    while (json.size() % 4 != 0) json.append(' ');   // GLB chunks are padded to 4 bytes
    auto u32 = [](quint32 v) {
        QByteArray b(4, '\0');
        for (int k = 0; k < 4; ++k) b[k] = char((v >> (8 * k)) & 0xFF);
        return b;
    };
    QByteArray glb = QByteArrayLiteral("glTF") + u32(2) + u32(quint32(12 + 8 + json.size()))
                     + u32(quint32(json.size())) + QByteArrayLiteral("JSON") + json;
    // Written to a temporary file and renamed, so a half-written file is never loaded.
    const QString tmp = path + QStringLiteral(".tmp");
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly) || f.write(glb) != glb.size()) {
        if (err) *err = QStringLiteral("could not write the empty object model to ") + tmp;
        return false;
    }
    f.close();
    QFile::remove(path);
    if (!QFile::rename(tmp, path)) {
        if (err) *err = QStringLiteral("could not move the empty object model to ") + path;
        return false;
    }
    return true;
}
void readFloats(const QJsonValue& v, float* out, int n) {
    const QJsonArray a = v.toArray();
    for (int i = 0; i < n && i < a.size(); ++i) out[i] = float(a[i].toDouble(out[i]));
}

std::vector<float> readLodSwitches(const QJsonValue& v) {
    std::vector<float> raw;
    for (const QJsonValue& x : v.toArray()) raw.push_back(float(x.toDouble(0.0)));
    return ukiyo::normalizeLodSwitches(raw);
}

QJsonArray lodSwitchesToJson(const std::vector<float>& switches) {
    QJsonArray a;
    for (float s : switches) a.append(double(s));
    return a;
}

std::optional<ukiyo::TerrainSettings> readTerrain(const QJsonValue& v, const QString& projectRoot) {
    if (!v.isObject()) return std::nullopt;
    const QJsonObject o = v.toObject();
    ukiyo::TerrainSettings t;
    t.heightmap = QDir::cleanPath(QDir(projectRoot).absoluteFilePath(o.value(QLatin1String("heightmap")).toString())).toStdString();
    t.size = uint32_t(std::max(2, o.value(QLatin1String("size")).toInt(1025)));
    t.cellSize = std::clamp(float(o.value(QLatin1String("cellSize")).toDouble(1.0)), 0.01f, 1000.0f);
    t.heightScale = std::clamp(float(o.value(QLatin1String("heightScale")).toDouble(10.0)), 0.0f, 10000.0f);
    // Layers (step 2). A missing or empty list keeps the default layers.
    const QJsonArray layers = o.value(QLatin1String("layers")).toArray();
    if (!layers.isEmpty()) {
        t.layers.clear();
        for (const QJsonValue& lv : layers) {
            if (t.layers.size() >= ukiyo::kTerrainMaxLayers) break;
            const QJsonObject lo = lv.toObject();
            ukiyo::TerrainLayer L;
            L.name = lo.value(QLatin1String("name")).toString(QStringLiteral("layer")).toStdString();
            const QJsonArray c = lo.value(QLatin1String("colour")).toArray();
            if (c.size() == 3)
                L.colour = glm::vec3(float(c[0].toDouble()), float(c[1].toDouble()), float(c[2].toDouble()));
            L.roughness = std::clamp(float(lo.value(QLatin1String("roughness")).toDouble(0.8)), 0.0f, 1.0f);
            L.heightMin = std::clamp(float(lo.value(QLatin1String("heightMin")).toDouble(0.0)), 0.0f, 1.0f);
            L.heightMax = std::clamp(float(lo.value(QLatin1String("heightMax")).toDouble(1.0)), 0.0f, 1.0f);
            L.heightFade = std::clamp(float(lo.value(QLatin1String("heightFade")).toDouble(0.1)), 0.0f, 1.0f);
            L.slopeMin = std::clamp(float(lo.value(QLatin1String("slopeMin")).toDouble(0.0)), 0.0f, 90.0f);
            L.slopeMax = std::clamp(float(lo.value(QLatin1String("slopeMax")).toDouble(90.0)), 0.0f, 90.0f);
            L.slopeFade = std::clamp(float(lo.value(QLatin1String("slopeFade")).toDouble(5.0)), 0.0f, 90.0f);
            L.tileMetres = std::clamp(float(lo.value(QLatin1String("tile")).toDouble(8.0)), 0.5f, 1000.0f);
            // Image files, relative to the project. Missing keys mean no map.
            const QString dif = lo.value(QLatin1String("diffuseMap")).toString();
            const QString nrm = lo.value(QLatin1String("normalMap")).toString();
            const QString rgh = lo.value(QLatin1String("roughnessMap")).toString();
            if (!dif.isEmpty()) L.diffuseFile = QDir::cleanPath(QDir(projectRoot).absoluteFilePath(dif)).toStdString();
            if (!nrm.isEmpty()) L.normalFile = QDir::cleanPath(QDir(projectRoot).absoluteFilePath(nrm)).toStdString();
            if (!rgh.isEmpty()) L.roughnessFile = QDir::cleanPath(QDir(projectRoot).absoluteFilePath(rgh)).toStdString();
            t.layers.push_back(L);
        }
    }
    // Foliage (step 3). Missing keys keep the defaults. Any shape other than "cone" is read as a tuft.
    const QJsonArray foliage = o.value(QLatin1String("foliage")).toArray();
    for (const QJsonValue& fv : foliage) {
        if (t.foliage.size() >= ukiyo::kFoliageMaxTypes) break;
        if (!fv.isObject()) continue;
        const QJsonObject fo = fv.toObject();
        ukiyo::FoliageType F;
        F.name = fo.value(QLatin1String("name")).toString(QStringLiteral("foliage")).toStdString();
        F.shape = fo.value(QLatin1String("shape")).toString() == QLatin1String("cone") ? ukiyo::FoliageShape::Cone
                                                                                          : ukiyo::FoliageShape::Tuft;
        F.density = std::clamp(float(fo.value(QLatin1String("density")).toDouble(0.5)), 0.0f, 1.0f);
        const QString dmap = fo.value(QLatin1String("densityMap")).toString();
        if (!dmap.isEmpty()) F.densityMap = QDir::cleanPath(QDir(projectRoot).absoluteFilePath(dmap)).toStdString();
        F.heightMin = std::clamp(float(fo.value(QLatin1String("heightMin")).toDouble(0.0)), 0.0f, 1.0f);
        F.heightMax = std::clamp(float(fo.value(QLatin1String("heightMax")).toDouble(1.0)), 0.0f, 1.0f);
        F.slopeMax = std::clamp(float(fo.value(QLatin1String("slopeMax")).toDouble(35.0)), 0.0f, 90.0f);
        F.scaleMin = std::clamp(float(fo.value(QLatin1String("scaleMin")).toDouble(0.8)), 0.01f, 100.0f);
        F.scaleMax = std::clamp(float(fo.value(QLatin1String("scaleMax")).toDouble(1.2)), 0.01f, 100.0f);
        F.seed = uint32_t(std::clamp<qint64>(fo.value(QLatin1String("seed")).toInteger(1), 0, 0xFFFFFFFFLL));
        F.fadeStart = std::clamp(float(fo.value(QLatin1String("fadeStart")).toDouble(60.0)), 0.0f, 10000.0f);
        F.fadeEnd = std::clamp(float(fo.value(QLatin1String("fadeEnd")).toDouble(90.0)), 0.0f, 10000.0f);
        const QJsonArray c = fo.value(QLatin1String("tint")).toArray();
        if (c.size() == 3)
            F.tint = glm::clamp(glm::vec3(float(c[0].toDouble()), float(c[1].toDouble()), float(c[2].toDouble())),
                                glm::vec3(0.0f), glm::vec3(1.0f));
        t.foliage.push_back(F);
    }
    // Layer masks (item 18, step 4): up to two RGBA images, relative to the project.
    for (const QJsonValue& mv : o.value(QLatin1String("masks")).toArray()) {
        if (t.maskFiles.size() >= 2) break;
        const QString mf = mv.toString();
        if (!mf.isEmpty()) t.maskFiles.push_back(QDir::cleanPath(QDir(projectRoot).absoluteFilePath(mf)).toStdString());
    }
    return t;
}

QJsonObject terrainToJson(const ukiyo::TerrainSettings& t, const QString& projectRoot) {
    QJsonObject o;
    o[QStringLiteral("heightmap")] = QDir(projectRoot).relativeFilePath(QString::fromStdString(t.heightmap));
    o[QStringLiteral("size")] = int(t.size);
    o[QStringLiteral("cellSize")] = double(t.cellSize);
    o[QStringLiteral("heightScale")] = double(t.heightScale);
    QJsonArray layers;
    for (const ukiyo::TerrainLayer& L : t.layers) {
        QJsonObject lo;
        lo[QStringLiteral("name")] = QString::fromStdString(L.name);
        lo[QStringLiteral("colour")] = QJsonArray{double(L.colour.r), double(L.colour.g), double(L.colour.b)};
        lo[QStringLiteral("roughness")] = double(L.roughness);
        lo[QStringLiteral("heightMin")] = double(L.heightMin);
        lo[QStringLiteral("heightMax")] = double(L.heightMax);
        lo[QStringLiteral("heightFade")] = double(L.heightFade);
        lo[QStringLiteral("slopeMin")] = double(L.slopeMin);
        lo[QStringLiteral("slopeMax")] = double(L.slopeMax);
        lo[QStringLiteral("slopeFade")] = double(L.slopeFade);
        lo[QStringLiteral("tile")] = double(L.tileMetres);
        if (!L.diffuseFile.empty()) lo[QStringLiteral("diffuseMap")] = QDir(projectRoot).relativeFilePath(QString::fromStdString(L.diffuseFile));
        if (!L.normalFile.empty()) lo[QStringLiteral("normalMap")] = QDir(projectRoot).relativeFilePath(QString::fromStdString(L.normalFile));
        if (!L.roughnessFile.empty()) lo[QStringLiteral("roughnessMap")] = QDir(projectRoot).relativeFilePath(QString::fromStdString(L.roughnessFile));
        layers.append(lo);
    }
    o[QStringLiteral("layers")] = layers;
    if (!t.foliage.empty()) {
        QJsonArray foliage;
        for (const ukiyo::FoliageType& F : t.foliage) {
            QJsonObject fo;
            fo[QStringLiteral("name")] = QString::fromStdString(F.name);
            fo[QStringLiteral("shape")] = QStringLiteral("tuft");
            if (F.shape == ukiyo::FoliageShape::Cone) fo[QStringLiteral("shape")] = QStringLiteral("cone");
            fo[QStringLiteral("density")] = double(F.density);
            if (!F.densityMap.empty()) fo[QStringLiteral("densityMap")] = QDir(projectRoot).relativeFilePath(QString::fromStdString(F.densityMap));
            fo[QStringLiteral("heightMin")] = double(F.heightMin);
            fo[QStringLiteral("heightMax")] = double(F.heightMax);
            fo[QStringLiteral("slopeMax")] = double(F.slopeMax);
            fo[QStringLiteral("scaleMin")] = double(F.scaleMin);
            fo[QStringLiteral("scaleMax")] = double(F.scaleMax);
            fo[QStringLiteral("seed")] = double(F.seed);
            fo[QStringLiteral("fadeStart")] = double(F.fadeStart);
            fo[QStringLiteral("fadeEnd")] = double(F.fadeEnd);
            fo[QStringLiteral("tint")] = QJsonArray{double(F.tint.r), double(F.tint.g), double(F.tint.b)};
            foliage.append(fo);
        }
        o[QStringLiteral("foliage")] = foliage;
    }
    if (!t.maskFiles.empty()) {
        QJsonArray masks;
        for (const std::string& f : t.maskFiles) masks.append(QDir(projectRoot).relativeFilePath(QString::fromStdString(f)));
        o[QStringLiteral("masks")] = masks;
    }
    return o;
}

void parseWater(const QJsonObject& root, ukiyo::WaterVolume* out, bool* present) {
    const QJsonValue wv = root.value(QLatin1String("water"));
    *present = wv.isObject();
    if (!*present) return;
    const QJsonObject o = wv.toObject();
    ukiyo::WaterVolume w;
    float c[2] = {w.centre.x, w.centre.y};
    readFloats(o.value("centre"), c, 2);
    w.centre = glm::vec2(c[0], c[1]);
    float h[2] = {w.half.x, w.half.y};
    readFloats(o.value("half"), h, 2);
    w.half = glm::vec2(std::clamp(h[0], 0.5f, 500.0f), std::clamp(h[1], 0.5f, 500.0f));
    w.level = float(o.value("level").toDouble(w.level));
    float shallow[3] = {w.shallow.r, w.shallow.g, w.shallow.b};
    readFloats(o.value("shallow"), shallow, 3);
    w.shallow = glm::vec3(shallow[0], shallow[1], shallow[2]);
    float deep[3] = {w.deep.r, w.deep.g, w.deep.b};
    readFloats(o.value("deep"), deep, 3);
    w.deep = glm::vec3(deep[0], deep[1], deep[2]);
    w.absorption = std::clamp(float(o.value("absorption").toDouble(w.absorption)), 0.0f, 5.0f);
    w.foamWidth = std::clamp(float(o.value("foamWidth").toDouble(w.foamWidth)), 0.01f, 5.0f);
    w.refraction = std::clamp(float(o.value("refraction").toDouble(w.refraction)), 0.0f, 0.2f);
    w.density = std::clamp(float(o.value("density").toDouble(w.density)), 1.0f, 20000.0f);
    const QJsonArray waves = o.value("waves").toArray();
    for (int i = 0; i < 4 && i < waves.size(); ++i) {
        const QJsonObject q = waves[i].toObject();
        ukiyo::WaterWave& wave = w.waves[i];
        float d[2] = {wave.dir.x, wave.dir.y};
        readFloats(q.value("dir"), d, 2);
        wave.dir = glm::vec2(d[0], d[1]);
        wave.wavelength = std::clamp(float(q.value("wavelength").toDouble(wave.wavelength)), 0.5f, 200.0f);
        wave.amplitude = std::clamp(float(q.value("amplitude").toDouble(wave.amplitude)), 0.0f, 5.0f);
        wave.speed = std::clamp(float(q.value("speed").toDouble(wave.speed)), 0.0f, 4.0f);
        wave.steepness = std::clamp(float(q.value("steepness").toDouble(wave.steepness)), 0.0f, 1.0f);
    }
    *out = w;
}

QJsonObject waterToJson(const ukiyo::WaterVolume& v) {
    auto floats = [](std::initializer_list<float> xs) {
        QJsonArray a;
        for (float x : xs) a.append(double(x));
        return a;
    };
    QJsonObject o;
    o["centre"] = floats({v.centre.x, v.centre.y});
    o["half"] = floats({v.half.x, v.half.y});
    o["level"] = double(v.level);
    QJsonArray waves;
    for (const ukiyo::WaterWave& w : v.waves) {
        QJsonObject q;
        q["dir"] = floats({w.dir.x, w.dir.y});
        q["wavelength"] = double(w.wavelength);
        q["amplitude"] = double(w.amplitude);
        q["speed"] = double(w.speed);
        q["steepness"] = double(w.steepness);
        waves.append(q);
    }
    o["waves"] = waves;
    o["shallow"] = floats({v.shallow.r, v.shallow.g, v.shallow.b});
    o["deep"] = floats({v.deep.r, v.deep.g, v.deep.b});
    o["absorption"] = double(v.absorption);
    o["foamWidth"] = double(v.foamWidth);
    o["refraction"] = double(v.refraction);
    o["density"] = double(v.density);
    return o;
}

void parseSceneEntries(const QJsonObject& root, const QString& projectRoot, SceneContents* out) {
    out->active = std::max(0, root.value(QLatin1String("active")).toInt(0));
    parseWater(root, &out->water, &out->hasWater);
    std::vector<SceneEntry>& entries = out->entries;
    const QJsonArray models = root.value("models").toArray();
    for (const QJsonValue& v : models) {
        const QJsonObject o = v.toObject();
        SceneEntry e;
        e.components = systems_json::fromJson(o.value("components").toObject(), projectRoot);
        e.lodSwitches = readLodSwitches(o.value(QLatin1String("lod")));
        e.terrain = readTerrain(o.value(QLatin1String("terrain")), projectRoot);
        const QString prefabRel = o.value("prefab").toString();
        if (!prefabRel.isEmpty()) {
            // The prefab gives the model and the placement. Overridden keys replace its values.
            e.prefab = QDir::cleanPath(QDir(projectRoot).absoluteFilePath(prefabRel));
            e.prefabId = o.value("prefabId").toString();
            // The node the instance shows gives its model and placement, nested prefabs included.
            std::vector<prefab::FlatNode> nodes;
            QString err;
            const prefab::FlatNode* node = nullptr;
            if (prefab::flatten(e.prefab, projectRoot, &nodes, &err)) node = prefab::findNode(nodes, e.prefabId);
            if (!node || node->model.isEmpty()) {
                out->errors.append("error: " + (err.isEmpty() ? e.prefabId + " is not a model in " + QFileInfo(e.prefab).fileName() : err));
                continue;
            }
            e.path = QDir(projectRoot).absoluteFilePath(node->model);
            for (int k = 0; k < 3; ++k) e.position[k] = node->world.pos[k];
            e.yawDeg = node->world.yaw;
            const QJsonObject ov = o.value("overrides").toObject();
            if (ov.contains("translation")) {
                readFloats(ov.value("translation"), e.position, 3);
                e.overrides << QStringLiteral("translation");
            }
            if (ov.contains("yawDeg")) {
                e.yawDeg = float(ov.value("yawDeg").toDouble());
                e.overrides << QStringLiteral("yawDeg");
            }
        } else if (o.value("empty").toBool()) {
            QString emptyErr;
            if (!ensureEmptyModelFile(&emptyErr)) {
                out->errors.append("error: " + emptyErr);
                continue;
            }
            e.path = emptyModelPath();
            readFloats(o.value("position"), e.position, 3);
            e.yawDeg = float(o.value("yawDeg").toDouble());
        } else {
            e.path = QDir(projectRoot).absoluteFilePath(o.value("path").toString());
            readFloats(o.value("position"), e.position, 3);
            e.yawDeg = float(o.value("yawDeg").toDouble());
        }
        e.name = o.value("name").toString();
        entries.push_back(e);
    }
    if (entries.empty() && !root.value("model").toString().isEmpty() && models.isEmpty()) {
        SceneEntry e;
        e.path = QDir(projectRoot).absoluteFilePath(root.value("model").toString());
        entries.push_back(e);
    }
}

bool readSceneFile(const QString& path, const QString& projectRoot, SceneContents* out, QString* err) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (err) *err = QStringLiteral("could not open ") + path;
        return false;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.value(QLatin1String("format")).toString() != QLatin1String("ukiyo-scene")) {
        if (err) *err = path + QStringLiteral(" is not a Ukiyo scene");
        return false;
    }
    parseSceneEntries(root, projectRoot, out);
    return true;
}

} // namespace editor::scene_file
