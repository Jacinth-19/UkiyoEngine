// .ukprefab documents: a root object with a transform, optional model, and children. A child is a model, a
// container, or a reference to another prefab (nesting). See research/prefab_system_design.md.
#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <vector>

namespace editor::prefab {

// The root of a prefab, as the editor reads it. Multi-object prefabs use flatten() below.
struct Prefab {
    QString name;
    QString rootId;          // stable id of the root node; overrides refer to nodes by key
    QString model;           // components.mesh.model of the root, relative to the project root (may be empty)
    float position[3] = {0.0f, 0.0f, 0.0f};
    float yawDeg = 0.0f;
    QJsonObject doc;         // the whole document as read; write() keeps everything it does not own
};

bool read(const QString& absPath, Prefab* out, QString* error);
// Writes position and yawDeg into the root of p.doc, then saves the document.
bool write(const QString& absPath, const Prefab& p, QString* error);

// A placement: translation and a yaw about +Y, the same convention as the engine's placementOf.
struct Pose {
    float pos[3] = {0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;
};
Pose compose(const Pose& a, const Pose& b);       // a * b: b expressed in a's frame
Pose inverse(const Pose& a);
Pose localOf(const Pose& parent, const Pose& world);  // the local pose L with compose(parent, L) == world

// Limit on nested prefab references, counted in levels below the top file. Keeps the work bounded.
constexpr int kMaxNestDepth = 8;

// One node of a flattened prefab, parents before children. Every node, containers included, has a key.
struct FlatNode {
    QString key;             // root id, then child ids joined with '/'. A referenced prefab's root uses the
                             // key of the node that references it.
    QString parentKey;       // empty for the top root
    QString model;           // project-relative model, empty for a container or a reference node
    QString ownerFile;       // absolute path of the file that stores this node's transform
    QStringList ownerPath;   // ids from that file's root to the node; empty means the file's root
    Pose local;              // the owner's stored transform, relative to the parent
    Pose inner;              // offset of a referenced prefab's root (identity otherwise)
    Pose world;              // placement in the top prefab's space: compose(compose(parent, local), inner)
    int depth = 0;           // nesting level: 0 for the top file
};

// Flattens a prefab and everything it references. Fails on a cycle, a missing file, or nesting deeper than
// kMaxNestDepth. projectRoot resolves the model and prefab paths.
bool flatten(const QString& absPath, const QString& projectRoot, std::vector<FlatNode>* out, QString* error);
// The same, for a document that is not saved yet (used to check an edit before writing it).
bool flattenDoc(const QJsonObject& doc, const QString& absPath, const QString& projectRoot,
                std::vector<FlatNode>* out, QString* error);

bool readDoc(const QString& absPath, QJsonObject* doc, QString* error);
bool writeDoc(const QString& absPath, const QJsonObject& doc, QString* error);
// Sets translation and yawDeg on the node at ownerPath in doc. Returns false if no node has that path.
bool setNodeLocal(QJsonObject& doc, const QStringList& ownerPath, const Pose& local);
// Variant edits. A reference node stores "overrides" (path inside the referenced prefab -> translation and yawDeg)
// and "removed" (paths of objects to leave out). The paths use ids joined with '/'. A parent file's edits apply
// when the reference is flattened, and a path through a nested reference is handed down to that reference.
// setOverride and addRemoval write those edits into the reference at refPath, the ids from the root of doc.
bool setOverride(QJsonObject& doc, const QStringList& refPath, const QString& rel, const Pose& local);
bool addRemoval(QJsonObject& doc, const QStringList& refPath, const QString& rel);
// The nearest reference node above n that is owned by file, or null. Such a node brought n into the file.
const FlatNode* ownerRef(const std::vector<FlatNode>& nodes, const FlatNode& n, const QString& file);
// Removes the node at ownerPath, and everything under it. Fails on an empty path or a missing node.
bool removeNode(QJsonObject& doc, const QStringList& ownerPath);
// Appends a model child to the root, at a local pose. The caller makes sure the id is unused.
bool appendModelChild(QJsonObject& doc, const QString& id, const QString& model, const Pose& local);

const FlatNode* findNode(const std::vector<FlatNode>& nodes, const QString& key);

} // namespace editor::prefab
