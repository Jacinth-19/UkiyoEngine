#include "render_worker.h"

#include "anim_file.h"

#include <glm/gtc/quaternion.hpp>
#include <QElapsedTimer>

namespace editor {

RenderWorker::RenderWorker(QObject* parent) : QObject(parent) {}

bool RenderWorker::init(QString* error) {
    return session_.init(error);
}

bool RenderWorker::loadLibrary(const QString& path, QString* error, std::vector<ClipInfo>* clips) {
    if (!session_.loadAnimLibrary(path, error)) return false;
    // Keyframe and event edits beside the library. A bad file is reported by the editor, not here.
    ukiyo::ClipEdits edits;
    QString editError;
    if (animfile::readEdits(path, &edits, &editError)) session_.setClipEdits(edits);
    clips->clear();
    clips->push_back(ClipInfo{QString(), QStringLiteral("Idle loop"), 0.0f});
    for (const std::string& n : session_.clipNames()) {
        const float d = session_.clipDuration(n);
        const QString name = QString::fromStdString(n);
        clips->push_back(ClipInfo{name, QString("%1   (%2 s)").arg(name).arg(d, 0, 'f', 2), d});
    }
    return true;
}

void RenderWorker::setClipEdits(const ukiyo::ClipEdits& edits) {
    session_.setClipEdits(edits);
}

void RenderWorker::libraryData(ukiyo::SourceRig* rig, std::vector<ukiyo::AnimClip>* clips) const {
    session_.libraryData(rig, clips);
}

void RenderWorker::describeActive(ModelInfo* info) {
    // One render builds the world transforms that framing and focusing read.
    session_.render(ukiyo::OrbitCamera{}, 0.0f, 0.0f, ukiyo::RenderSettings{});

    *info = ModelInfo{};
    ukiyo::Model* m = session_.model();
    if (!m) return;

    info->nodes.reserve(m->nodes.size());
    for (const ukiyo::Node& nd : m->nodes) {
        NodeInfo n;
        n.name = QString::fromStdString(nd.name);
        n.parent = nd.parent;
        n.hasMatrix = nd.hasMatrix;
        const glm::vec3 deg = glm::degrees(glm::eulerAngles(nd.baseRotation));
        n.translation[0] = nd.translation.x;
        n.translation[1] = nd.translation.y;
        n.translation[2] = nd.translation.z;
        n.rotationDeg[0] = deg.x;
        n.rotationDeg[1] = deg.y;
        n.rotationDeg[2] = deg.z;
        n.scale[0] = nd.scale.x;
        n.scale[1] = nd.scale.y;
        n.scale[2] = nd.scale.z;
        info->nodes.push_back(n);
    }
    for (const ukiyo::DrawItem& d : m->draws)
        if (d.node >= 0 && d.node < static_cast<int>(info->nodes.size())) info->nodes[d.node].hasMesh = true;
    for (const ukiyo::DrawItem& d : m->draws) {
        if (d.node < 0 || d.node >= static_cast<int>(info->nodes.size())) continue;
        if (d.primitive < 0 || d.primitive >= static_cast<int>(m->primitives.size())) continue;
        const ukiyo::Primitive& p = m->primitives[d.primitive];
        SlotInfo s;
        s.primitive = d.primitive;
        if (p.material) {
            s.material = QString::fromStdString(p.material->name);
            s.doubleSided = p.material->doubleSided;
            s.blend = p.material->blend;
            s.mask = p.material->mask;
        }
        s.triangles = p.indexCount / 3;
        info->nodes[d.node].materialSlots.push_back(s);
    }

    auto it = m->humanoid.find("head");
    if (it != m->humanoid.end()) {
        info->headNode = it->second;
        info->headY = session_.nodeWorldPosition(it->second).y;
    }
}

bool RenderWorker::loadModel(const QString& path, QString* error, ModelInfo* info) {
    if (!session_.loadModel(path, error)) return false;
    describeActive(info);
    return true;
}

bool RenderWorker::addModel(const QString& path, const float position[3], float yawDeg, QString* error,
                            ModelInfo* info, int* index) {
    if (!session_.addModel(path, position, yawDeg, error, index)) return false;
    describeActive(info);
    return true;
}

bool RenderWorker::activateInstance(int index, ModelInfo* info) {
    if (!session_.setActiveInstance(index)) return false;
    describeActive(info);
    return true;
}

bool RenderWorker::setInstancePlacement(int index, const float position[3], float yawDeg) {
    return session_.setInstancePlacement(index, position, yawDeg);
}

bool RenderWorker::setInstanceLink(int index, const QString& prefab, const QString& prefabId,
                                   const QStringList& overrides) {
    std::set<std::string> keys;
    for (const QString& k : overrides) keys.insert(k.toStdString());
    return session_.setInstanceLink(index, prefab.toStdString(), prefabId.toStdString(), keys);
}

std::vector<InstanceInfo> RenderWorker::instances() const {
    std::vector<InstanceInfo> out;
    for (int i = 0; i < session_.instanceCount(); ++i) {
        const EngineSession::PlacedModel p = session_.instanceInfo(i);
        InstanceInfo in;
        in.path = QString::fromStdString(p.path);
        for (int k = 0; k < 3; ++k) in.position[k] = p.position[k];
        in.yawDeg = p.yawDeg;
        in.prefab = QString::fromStdString(p.prefab);
        in.prefabId = QString::fromStdString(p.prefabId);
        for (const std::string& k : p.overrides) in.overrides << QString::fromStdString(k);
        in.name = QString::fromStdString(p.name);
        in.lodSwitches = p.lodSwitches;
        in.terrain = p.terrain;
        in.components = session_.instanceComponents(i);
        out.push_back(in);
    }
    return out;
}

void RenderWorker::playClip(const QString& name) {
    session_.playClip(name.toStdString());
}

QString RenderWorker::setNodeGraphMaterial(int node, const QString& ukmatPath) {
    return session_.setNodeGraphMaterial(node, ukmatPath);
}

QString RenderWorker::setNodeMmdToon(int node, bool on) {
    return session_.setNodeMmdToon(node, on);
}

QString RenderWorker::setGradingLut(const QString& absPath) {
    return session_.setGradingLut(absPath);
}

QString RenderWorker::refreshGraphMaterials() {
    return session_.refreshGraphMaterials();
}

void RenderWorker::setRestPose(int node, const float* t, const float* rotDeg, const float* s) {
    ukiyo::Model* m = session_.model();
    if (!m || node < 0 || node >= static_cast<int>(m->nodes.size())) return;
    ukiyo::Node& nd = m->nodes[node];
    if (nd.hasMatrix) return;
    nd.translation = glm::vec3(t[0], t[1], t[2]);
    nd.baseRotation = glm::quat(glm::radians(glm::vec3(rotDeg[0], rotDeg[1], rotDeg[2])));
    nd.scale = glm::vec3(s[0], s[1], s[2]);
}

glm::vec3 RenderWorker::nodeWorldPosition(int node) const {
    return session_.nodeWorldPosition(node);
}

void RenderWorker::renderFrame(const RenderRequest& req) {
    QElapsedTimer timer;
    timer.start();
    if (req.width > 0 && req.height > 0) session_.setRenderSize(req.width, req.height);
    QImage image = session_.render(req.cam, req.time, req.dt, req.settings);
    reportScene();   // a scene switch in the play step comes before the frame, so the GUI sees it first
    const float clipLength = session_.clipDuration(session_.currentClip());
    emit frameReady(image, req.game, clipLength, double(timer.nsecsElapsed()) / 1.0e6, session_.lastDrawCalls(), session_.lastTriangles());
}

void RenderWorker::reportScene() {
    if (session_.hasSceneNotes()) emit sceneChanged();
}

void RenderWorker::shutdown() {
    session_.shutdown();
}

} // namespace editor
