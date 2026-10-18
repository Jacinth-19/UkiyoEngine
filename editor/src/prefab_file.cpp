#include "prefab_file.h"

#include <functional>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

#include <cmath>

namespace editor::prefab {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Rotates v about +Y by deg, the same as glm::rotate(deg, +Y) applied to a point.
void rotY(float deg, const float v[3], float out[3]) {
    const double a = double(deg) * kPi / 180.0;
    const float c = float(std::cos(a));
    const float s = float(std::sin(a));
    out[0] = c * v[0] + s * v[2];
    out[1] = v[1];
    out[2] = -s * v[0] + c * v[2];
}

Pose readPose(const QJsonObject& o) {
    Pose p;
    const QJsonArray t = o.value(QStringLiteral("translation")).toArray();
    for (int k = 0; k < 3 && k < t.size(); ++k) p.pos[k] = float(t[k].toDouble());
    p.yaw = float(o.value(QStringLiteral("yawDeg")).toDouble());
    return p;
}

QString modelOf(const QJsonObject& node) {
    return node.value(QStringLiteral("components")).toObject()
        .value(QStringLiteral("mesh")).toObject()
        .value(QStringLiteral("model")).toString();
}

bool applyEdit(QJsonObject& node, const QStringList& path, int i, const QJsonObject* value);

QString keyFor(const QString& parentKey, const QString& id) {
    return parentKey.isEmpty() ? id : parentKey + QLatin1Char('/') + id;
}

// Walks one node and its children. ownerPath is the node's path in `file`, and the children's paths
// extend it. A reference node takes the referenced file's root as its own, and that file's children
// are walked with paths relative to it.
bool walk(const QJsonObject& node, const QString& file, const QString& projectRoot, const QString& parentKey,
          const Pose& parentWorld, const QStringList& ownerPath, const QStringList& stack, int depth,
          std::vector<FlatNode>* out, QString* error) {
    const QString id = node.value(QStringLiteral("id")).toString();
    const QString key = keyFor(parentKey, id);
    const QString ref = node.value(QStringLiteral("prefab")).toString();

    if (!ref.isEmpty()) {
        const QString innerFile = QDir::cleanPath(QDir(projectRoot).absoluteFilePath(ref));
        if (stack.contains(innerFile)) {
            if (error) *error = QStringLiteral("prefab cycle: %1 refers back to itself through %2").arg(QFileInfo(file).fileName(), QFileInfo(innerFile).fileName());
            return false;
        }
        if (depth + 1 > kMaxNestDepth) {
            if (error) *error = QStringLiteral("nested prefabs go deeper than %1 levels at %2").arg(kMaxNestDepth).arg(key);
            return false;
        }
        QJsonObject innerDoc;
        if (!readDoc(innerFile, &innerDoc, error)) return false;
        QJsonObject innerRoot = innerDoc.value(QStringLiteral("root")).toObject();
        if (innerRoot.isEmpty()) {
            if (error) *error = innerFile + " has no root";
            return false;
        }
        // This file's edits to the referenced prefab: objects it leaves out, then transforms it overrides.
        for (const QJsonValue& r : node.value(QStringLiteral("removed")).toArray())
            applyEdit(innerRoot, r.toString().split(QLatin1Char('/')), 0, nullptr);
        const QJsonObject overrides = node.value(QStringLiteral("overrides")).toObject();
        for (auto it = overrides.begin(); it != overrides.end(); ++it) {
            const QJsonObject value = it.value().toObject();
            applyEdit(innerRoot, it.key().split(QLatin1Char('/')), 0, &value);
        }
        const Pose local = readPose(node);
        const Pose inner = readPose(innerRoot);
        const Pose world = compose(compose(parentWorld, local), inner);

        FlatNode fn;
        fn.key = key;
        fn.parentKey = parentKey;
        fn.model = modelOf(innerRoot);
        fn.ownerFile = file;
        fn.ownerPath = ownerPath;
        fn.local = local;
        fn.inner = inner;
        fn.world = world;
        fn.depth = depth;
        out->push_back(fn);

        QStringList innerStack = stack;
        innerStack << innerFile;
        const QJsonArray kids = innerRoot.value(QStringLiteral("children")).toArray();
        for (const QJsonValue& v : kids) {
            const QJsonObject child = v.toObject();
            const QString cid = child.value(QStringLiteral("id")).toString();
            if (!walk(child, innerFile, projectRoot, key, world, QStringList{cid}, innerStack, depth + 1, out, error))
                return false;
        }
        return true;
    }

    const Pose local = readPose(node);
    const Pose world = compose(parentWorld, local);
    FlatNode fn;
    fn.key = key;
    fn.parentKey = parentKey;
    fn.model = modelOf(node);
    fn.ownerFile = file;
    fn.ownerPath = ownerPath;
    fn.local = local;
    fn.world = world;
    fn.depth = depth;
    out->push_back(fn);

    const QJsonArray kids = node.value(QStringLiteral("children")).toArray();
    for (const QJsonValue& v : kids) {
        const QJsonObject child = v.toObject();
        const QString cid = child.value(QStringLiteral("id")).toString();
        if (!walk(child, file, projectRoot, key, world, ownerPath + QStringList{cid}, stack, depth, out, error))
            return false;
    }
    return true;
}

// Finds the node at `path` (ids from the root) and sets its transform. The root has an empty path.
bool editAt(QJsonObject& node, const QStringList& path, int i, const Pose& local) {
    if (i == path.size()) {
        node[QStringLiteral("translation")] = QJsonArray{double(local.pos[0]), double(local.pos[1]), double(local.pos[2])};
        node[QStringLiteral("yawDeg")] = double(local.yaw);
        return true;
    }
    QJsonArray kids = node.value(QStringLiteral("children")).toArray();
    for (int k = 0; k < kids.size(); ++k) {
        QJsonObject child = kids[k].toObject();
        if (child.value(QStringLiteral("id")).toString() != path[i]) continue;
        if (!editAt(child, path, i + 1, local)) return false;
        kids[k] = child;
        node[QStringLiteral("children")] = kids;
        return true;
    }
    return false;
}

// Removes the child at path[i] under node, and the rest of the path below it.
bool removeAt(QJsonObject& node, const QStringList& path, int i) {
    QJsonArray kids = node.value(QStringLiteral("children")).toArray();
    for (int k = 0; k < kids.size(); ++k) {
        QJsonObject child = kids[k].toObject();
        if (child.value(QStringLiteral("id")).toString() != path[i]) continue;
        if (i == path.size() - 1) {
            kids.removeAt(k);
        } else {
            if (!removeAt(child, path, i + 1)) return false;
            kids[k] = child;
        }
        node[QStringLiteral("children")] = kids;
        return true;
    }
    return false;
}

// Calls fn on the node at path (ids below node). An empty path is node itself.
bool editNodeAt(QJsonObject& node, const QStringList& path, int i, const std::function<void(QJsonObject&)>& fn) {
    if (i == path.size()) {
        fn(node);
        return true;
    }
    QJsonArray kids = node.value(QStringLiteral("children")).toArray();
    for (int k = 0; k < kids.size(); ++k) {
        QJsonObject child = kids[k].toObject();
        if (child.value(QStringLiteral("id")).toString() != path[i]) continue;
        if (!editNodeAt(child, path, i + 1, fn)) return false;
        kids[k] = child;
        node[QStringLiteral("children")] = kids;
        return true;
    }
    return false;
}

// Applies one edit to the node at path below node: a transform (value != null) or a removal (value == null).
// A reference on the way takes the rest of the path into its own "overrides" or "removed".
bool applyEdit(QJsonObject& node, const QStringList& path, int i, const QJsonObject* value) {
    QJsonArray kids = node.value(QStringLiteral("children")).toArray();
    for (int k = 0; k < kids.size(); ++k) {
        QJsonObject child = kids[k].toObject();
        if (child.value(QStringLiteral("id")).toString() != path[i]) continue;
        if (i == path.size() - 1) {
            if (value) {
                child[QStringLiteral("translation")] = value->value(QStringLiteral("translation"));
                child[QStringLiteral("yawDeg")] = value->value(QStringLiteral("yawDeg"));
            } else {
                kids.removeAt(k);
                node[QStringLiteral("children")] = kids;
                return true;
            }
        } else if (!child.value(QStringLiteral("prefab")).toString().isEmpty()) {
            const QString rest = path.mid(i + 1).join(QLatin1Char('/'));
            if (value) {
                QJsonObject ov = child.value(QStringLiteral("overrides")).toObject();
                ov[rest] = *value;
                child[QStringLiteral("overrides")] = ov;
            } else {
                QJsonArray rem = child.value(QStringLiteral("removed")).toArray();
                rem.append(rest);
                child[QStringLiteral("removed")] = rem;
            }
        } else {
            if (!applyEdit(child, path, i + 1, value)) return false;
        }
        kids[k] = child;
        node[QStringLiteral("children")] = kids;
        return true;
    }
    return false;
}

} // namespace

bool read(const QString& absPath, Prefab* out, QString* error) {
    QJsonObject doc;
    if (!readDoc(absPath, &doc, error)) return false;
    if (doc.value(QStringLiteral("format")).toString() != QLatin1String("ukiyo-prefab")) {
        if (error) *error = absPath + " is not a Ukiyo prefab";
        return false;
    }
    const QJsonObject root = doc.value(QStringLiteral("root")).toObject();
    if (root.isEmpty()) {
        if (error) *error = absPath + " has no root";
        return false;
    }
    Prefab p;
    p.doc = doc;
    p.name = doc.value(QStringLiteral("name")).toString();
    p.rootId = root.value(QStringLiteral("id")).toString();
    p.model = modelOf(root);
    const Pose pose = readPose(root);
    for (int k = 0; k < 3; ++k) p.position[k] = pose.pos[k];
    p.yawDeg = pose.yaw;
    // A root without a model is valid when it holds children (a container of models or prefab references).
    if (p.model.isEmpty() && root.value(QStringLiteral("children")).toArray().isEmpty()) {
        if (error) *error = absPath + " has no mesh model and no children";
        return false;
    }
    *out = p;
    return true;
}

bool write(const QString& absPath, const Prefab& p, QString* error) {
    QJsonObject doc = p.doc;
    QJsonObject root = doc.value(QStringLiteral("root")).toObject();
    root[QStringLiteral("translation")] = QJsonArray{double(p.position[0]), double(p.position[1]), double(p.position[2])};
    root[QStringLiteral("yawDeg")] = double(p.yawDeg);
    doc[QStringLiteral("root")] = root;
    return writeDoc(absPath, doc, error);
}

Pose compose(const Pose& a, const Pose& b) {
    Pose r;
    float rot[3];
    rotY(a.yaw, b.pos, rot);
    for (int k = 0; k < 3; ++k) r.pos[k] = a.pos[k] + rot[k];
    r.yaw = a.yaw + b.yaw;
    return r;
}

Pose inverse(const Pose& a) {
    Pose r;
    const float p[3] = {a.pos[0], a.pos[1], a.pos[2]};
    float rot[3];
    rotY(-a.yaw, p, rot);
    for (int k = 0; k < 3; ++k) r.pos[k] = -rot[k];
    r.yaw = -a.yaw;
    return r;
}

Pose localOf(const Pose& parent, const Pose& world) {
    return compose(inverse(parent), world);
}

bool flatten(const QString& absPath, const QString& projectRoot, std::vector<FlatNode>* out, QString* error) {
    QJsonObject doc;
    if (!readDoc(absPath, &doc, error)) return false;
    return flattenDoc(doc, absPath, projectRoot, out, error);
}

bool flattenDoc(const QJsonObject& doc, const QString& absPath, const QString& projectRoot,
                std::vector<FlatNode>* out, QString* error) {
    const QString file = QDir::cleanPath(QFileInfo(absPath).absoluteFilePath());
    const QJsonObject root = doc.value(QStringLiteral("root")).toObject();
    if (root.isEmpty()) {
        if (error) *error = file + " has no root";
        return false;
    }
    out->clear();
    return walk(root, file, projectRoot, QString(), Pose(), QStringList(), QStringList{file}, 0, out, error);
}

bool readDoc(const QString& absPath, QJsonObject* doc, QString* error) {
    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = "could not open " + absPath;
        return false;
    }
    QJsonParseError perr;
    const QJsonDocument jd = QJsonDocument::fromJson(f.readAll(), &perr);
    if (perr.error != QJsonParseError::NoError || !jd.isObject()) {
        if (error) *error = absPath + " is not valid JSON";
        return false;
    }
    *doc = jd.object();
    return true;
}

bool writeDoc(const QString& absPath, const QJsonObject& doc, QString* error) {
    QFile f(absPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = "could not write " + absPath;
        return false;
    }
    f.write(QJsonDocument(doc).toJson(QJsonDocument::Indented));
    return true;
}

bool setNodeLocal(QJsonObject& doc, const QStringList& ownerPath, const Pose& local) {
    QJsonObject root = doc.value(QStringLiteral("root")).toObject();
    if (!editAt(root, ownerPath, 0, local)) return false;
    doc[QStringLiteral("root")] = root;
    return true;
}

bool removeNode(QJsonObject& doc, const QStringList& ownerPath) {
    if (ownerPath.isEmpty()) return false;   // the root cannot be removed this way
    QJsonObject root = doc.value(QStringLiteral("root")).toObject();
    if (!removeAt(root, ownerPath, 0)) return false;
    doc[QStringLiteral("root")] = root;
    return true;
}

bool appendModelChild(QJsonObject& doc, const QString& id, const QString& model, const Pose& local) {
    QJsonObject root = doc.value(QStringLiteral("root")).toObject();
    QJsonArray kids = root.value(QStringLiteral("children")).toArray();
    kids.append(QJsonObject{
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), id},
        {QStringLiteral("components"), QJsonObject{{QStringLiteral("mesh"), QJsonObject{{QStringLiteral("model"), model}}}}},
        {QStringLiteral("translation"), QJsonArray{double(local.pos[0]), double(local.pos[1]), double(local.pos[2])}},
        {QStringLiteral("yawDeg"), double(local.yaw)}});
    root[QStringLiteral("children")] = kids;
    doc[QStringLiteral("root")] = root;
    return true;
}

const FlatNode* findNode(const std::vector<FlatNode>& nodes, const QString& key) {
    for (const FlatNode& n : nodes)
        if (n.key == key) return &n;
    return nullptr;
}

const FlatNode* ownerRef(const std::vector<FlatNode>& nodes, const FlatNode& n, const QString& file) {
    const FlatNode* cur = &n;
    while (cur && !cur->parentKey.isEmpty()) {
        cur = findNode(nodes, cur->parentKey);
        if (cur && cur->ownerFile == file) return cur;
    }
    return nullptr;
}

bool setOverride(QJsonObject& doc, const QStringList& refPath, const QString& rel, const Pose& local) {
    QJsonObject root = doc.value(QStringLiteral("root")).toObject();
    const bool ok = editNodeAt(root, refPath, 0, [&](QJsonObject& ref) {
        QJsonObject ov = ref.value(QStringLiteral("overrides")).toObject();
        ov[rel] = QJsonObject{{QStringLiteral("translation"), QJsonArray{double(local.pos[0]), double(local.pos[1]), double(local.pos[2])}},
                              {QStringLiteral("yawDeg"), double(local.yaw)}};
        ref[QStringLiteral("overrides")] = ov;
    });
    if (ok) doc[QStringLiteral("root")] = root;
    return ok;
}

bool addRemoval(QJsonObject& doc, const QStringList& refPath, const QString& rel) {
    QJsonObject root = doc.value(QStringLiteral("root")).toObject();
    const bool ok = editNodeAt(root, refPath, 0, [&](QJsonObject& ref) {
        QJsonArray rem = ref.value(QStringLiteral("removed")).toArray();
        bool present = false;
        for (const QJsonValue& v : rem) present = present || v.toString() == rel;
        if (!present) rem.append(rel);
        ref[QStringLiteral("removed")] = rem;
        QJsonObject ov = ref.value(QStringLiteral("overrides")).toObject();
        ov.remove(rel);
        ref[QStringLiteral("overrides")] = ov;
    });
    if (ok) doc[QStringLiteral("root")] = root;
    return ok;
}

} // namespace editor::prefab
