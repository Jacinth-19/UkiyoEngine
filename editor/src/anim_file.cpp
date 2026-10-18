#include "anim_file.h"

#include <algorithm>
#include <cmath>

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace animfile {
namespace {

const char* pathName(ukiyo::AnimTrack::Path p) {
    switch (p) {
    case ukiyo::AnimTrack::Path::Translation: return "translation";
    case ukiyo::AnimTrack::Path::Rotation: return "rotation";
    case ukiyo::AnimTrack::Path::Scale: return "scale";
    }
    return "translation";
}

bool pathFrom(const QString& s, ukiyo::AnimTrack::Path* out) {
    if (s == "translation") *out = ukiyo::AnimTrack::Path::Translation;
    else if (s == "rotation") *out = ukiyo::AnimTrack::Path::Rotation;
    else if (s == "scale") *out = ukiyo::AnimTrack::Path::Scale;
    else return false;
    return true;
}

size_t componentsFor(ukiyo::AnimTrack::Path p) { return p == ukiyo::AnimTrack::Path::Rotation ? 4 : 3; }

QJsonArray floatArray(const std::vector<float>& v) {
    QJsonArray a;
    for (float f : v) a.append(double(f));
    return a;
}

std::vector<float> floatsFrom(const QJsonValue& v) {
    std::vector<float> out;
    for (const QJsonValue& x : v.toArray()) out.push_back(float(x.toDouble()));
    return out;
}

} // namespace

QString editsPath(const QString& libraryPath) {
    const QFileInfo fi(libraryPath);
    return fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() + QStringLiteral(".ukanim");
}

bool readEdits(const QString& libraryPath, ukiyo::ClipEdits* out, QString* error) {
    *out = ukiyo::ClipEdits{};
    const QString path = editsPath(libraryPath);
    if (!QFileInfo::exists(path)) return true;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("could not open ") + path;
        return false;
    }
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = QStringLiteral("%1 is not valid JSON: %2").arg(path, err.errorString());
        return false;
    }
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("format")).toString() != QLatin1String("ukiyo-anim")) {
        if (error) *error = QStringLiteral("%1 is not a Ukiyo animation edit file").arg(path);
        return false;
    }
    const QJsonObject clips = root.value(QStringLiteral("clips")).toObject();
    for (auto it = clips.constBegin(); it != clips.constEnd(); ++it) {
        const std::string clip = it.key().toStdString();
        const QJsonObject c = it.value().toObject();
        for (const QJsonValue& tv : c.value(QStringLiteral("tracks")).toArray()) {
            const QJsonObject t = tv.toObject();
            ukiyo::ClipEdits::TrackEdit te;
            te.clip = clip;
            te.node = t.value(QStringLiteral("node")).toString().toStdString();
            if (!pathFrom(t.value(QStringLiteral("path")).toString(), &te.path)) continue;
            te.interp = t.value(QStringLiteral("interp")).toString() == QLatin1String("step")
                            ? ukiyo::AnimTrack::Interp::Step
                            : ukiyo::AnimTrack::Interp::Linear;
            if (!t.value(QStringLiteral("removed")).toBool(false)) {
                te.times = floatsFrom(t.value(QStringLiteral("times")));
                te.values = floatsFrom(t.value(QStringLiteral("values")));
            }
            out->tracks.push_back(te);
        }
        if (c.contains(QStringLiteral("events"))) {
            ukiyo::ClipEdits::EventSet es;
            es.clip = clip;
            for (const QJsonValue& ev : c.value(QStringLiteral("events")).toArray()) {
                const QJsonObject o = ev.toObject();
                ukiyo::AnimEvent e;
                e.time = float(o.value(QStringLiteral("time")).toDouble());
                e.name = o.value(QStringLiteral("name")).toString().toStdString();
                es.events.push_back(e);
            }
            out->eventSets.push_back(es);
        }
    }
    return true;
}

bool writeEdits(const QString& libraryPath, const ukiyo::ClipEdits& edits, QString* error) {
    const QString path = editsPath(libraryPath);
    if (edits.tracks.empty() && edits.eventSets.empty()) {
        if (QFileInfo::exists(path) && !QFile::remove(path)) {
            if (error) *error = QStringLiteral("could not remove ") + path;
            return false;
        }
        return true;
    }
    QJsonObject clips;
    // Each clip gets one object with its tracks and events.
    QSet<QString> names;
    for (const auto& te : edits.tracks) names.insert(QString::fromStdString(te.clip));
    for (const auto& es : edits.eventSets) names.insert(QString::fromStdString(es.clip));
    for (const QString& key : names) {
        QJsonObject c;
        QJsonArray tracks;
        for (const auto& te : edits.tracks) {
            if (QString::fromStdString(te.clip) != key) continue;
            QJsonObject t;
            t[QStringLiteral("node")] = QString::fromStdString(te.node);
            t[QStringLiteral("path")] = QLatin1String(pathName(te.path));
            t[QStringLiteral("interp")] = QLatin1String(te.interp == ukiyo::AnimTrack::Interp::Step ? "step" : "linear");
            if (te.times.empty()) {
                t[QStringLiteral("removed")] = true;
            } else {
                QJsonArray times;
                for (float f : te.times) times.append(double(f));
                t[QStringLiteral("times")] = times;
                t[QStringLiteral("values")] = floatArray(te.values);
            }
            tracks.append(t);
        }
        c[QStringLiteral("tracks")] = tracks;
        for (const auto& es : edits.eventSets) {
            if (QString::fromStdString(es.clip) != key) continue;
            QJsonArray events;
            for (const ukiyo::AnimEvent& e : es.events) {
                QJsonObject o;
                o[QStringLiteral("time")] = double(e.time);
                o[QStringLiteral("name")] = QString::fromStdString(e.name);
                events.append(o);
            }
            c[QStringLiteral("events")] = events;
        }
        clips[key] = c;
    }
    QJsonObject root;
    root[QStringLiteral("format")] = QStringLiteral("ukiyo-anim");
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("library")] = QFileInfo(libraryPath).fileName();
    root[QStringLiteral("clips")] = clips;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QStringLiteral("could not write ") + path;
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

void setTrackEdit(ukiyo::ClipEdits* edits, const std::string& clip, const std::string& node,
                  ukiyo::AnimTrack::Path path, ukiyo::AnimTrack::Interp interp, std::vector<float> times,
                  std::vector<float> values) {
    for (auto& te : edits->tracks) {
        if (te.clip == clip && te.node == node && te.path == path) {
            te.interp = interp;
            te.times = std::move(times);
            te.values = std::move(values);
            return;
        }
    }
    ukiyo::ClipEdits::TrackEdit te;
    te.clip = clip;
    te.node = node;
    te.path = path;
    te.interp = interp;
    te.times = std::move(times);
    te.values = std::move(values);
    edits->tracks.push_back(std::move(te));
}

void setEventEdit(ukiyo::ClipEdits* edits, const std::string& clip, std::vector<ukiyo::AnimEvent> events) {
    for (auto& es : edits->eventSets) {
        if (es.clip == clip) {
            es.events = std::move(events);
            return;
        }
    }
    ukiyo::ClipEdits::EventSet es;
    es.clip = clip;
    es.events = std::move(events);
    edits->eventSets.push_back(std::move(es));
}

std::vector<float> keyValues(const ukiyo::AnimTrack& track) {
    const size_t comps = componentsFor(track.path);
    const bool cubic = track.interp == ukiyo::AnimTrack::Interp::Cubic;
    std::vector<float> out;
    for (size_t k = 0; k < track.times.size(); ++k) {
        const size_t slot = cubic ? k * 3 + 1 : k;
        for (size_t c = 0; c < comps; ++c) {
            const size_t i = slot * comps + c;
            out.push_back(i < track.values.size() ? track.values[i] : 0.0f);
        }
    }
    return out;
}

std::vector<float> valueAt(const ukiyo::AnimTrack& track, float t) {
    const size_t comps = componentsFor(track.path);
    const std::vector<float> keys = keyValues(track);
    const size_t n = track.times.size();
    if (n == 0) return std::vector<float>(comps, 0.0f);
    if (t <= track.times.front()) return std::vector<float>(keys.begin(), keys.begin() + comps);
    if (t >= track.times.back()) return std::vector<float>(keys.end() - comps, keys.end());
    size_t k = 0;
    while (k + 1 < n && track.times[k + 1] <= t) ++k;
    const float t0 = track.times[k], t1 = track.times[k + 1];
    const float s = t1 - t0 > 1e-9f ? (t - t0) / (t1 - t0) : 0.0f;
    std::vector<float> out(comps);
    for (size_t c = 0; c < comps; ++c) {
        const float a = keys[k * comps + c], b = keys[(k + 1) * comps + c];
        out[c] = track.interp == ukiyo::AnimTrack::Interp::Step ? a : a + (b - a) * s;
    }
    return out;
}

std::unique_ptr<ukiyo::AnimStateMachine> buildStateMachine(const ukiyo::AnimComponent& a,
                                                           const std::map<std::string, float>& durations) {
    using ukiyo::AnimStateMachine;
    auto sm = std::make_unique<AnimStateMachine>();
    for (const auto& [clip, seconds] : durations) sm->setClipDuration(clip, seconds);
    if (!a.custom) {
        // Three-state locomotion driven by the "speed" parameter (metres per second).
        sm->addState("Idle", a.idleClip);
        sm->addState("Walk", a.walkClip);
        sm->addState("Run", a.runClip);
        sm->setDefaultState("Idle");
        using Cond = AnimStateMachine::Condition;
        auto when = [](Cond::Op op, float v) {
            Cond c;
            c.param = "speed";
            c.op = op;
            c.value = v;
            return std::vector<Cond>{c};
        };
        // Order matters: the first transition whose conditions hold wins.
        sm->addTransition("Idle", "Walk", when(Cond::Op::Greater, a.walkSpeed), 0.2f, 0.1f);
        sm->addTransition("Walk", "Run", when(Cond::Op::Greater, a.runSpeed), 0.2f, 0.1f);
        sm->addTransition("Run", "Walk", when(Cond::Op::Less, a.runSpeed), 0.2f, 0.1f);
        sm->addTransition("Walk", "Idle", when(Cond::Op::Less, a.walkSpeed), 0.2f, 0.1f);
        return sm;
    }
    for (const ukiyo::AnimParamDef& p : a.params) {
        if (p.type == ukiyo::AnimParamDef::Type::Float) sm->setFloat(p.name, p.value);
        else if (p.type == ukiyo::AnimParamDef::Type::Bool) sm->setBool(p.name, p.value != 0.0f);
    }
    for (const ukiyo::AnimStateDef& s : a.states) {
        if (s.blend == ukiyo::BlendKind::None) sm->addState(s.name, s.clip);
        else sm->addBlendState(s.name, s.blend, s.paramX, s.paramY, s.motions);
    }
    if (!a.defaultState.empty()) sm->setDefaultState(a.defaultState);
    else if (!a.states.empty()) sm->setDefaultState(a.states.front().name);
    for (const ukiyo::AnimTransitionDef& t : a.transitions) sm->addTransition(t.from, t.to, t.when, t.fade, t.minDwell, t.opts);
    return sm;
}

QStringList validateGraph(const ukiyo::AnimComponent& a, const QStringList& clipNames) {
    QStringList problems;
    if (a.states.empty()) {
        problems << QStringLiteral("Add a state to start the graph.");
        return problems;
    }
    QSet<QString> states;
    QSet<QString> params;
    QSet<QString> floats;
    for (const ukiyo::AnimParamDef& p : a.params) {
        params.insert(QString::fromStdString(p.name));
        if (p.type == ukiyo::AnimParamDef::Type::Float) floats.insert(QString::fromStdString(p.name));
    }
    for (const ukiyo::AnimStateDef& s : a.states) {
        const QString name = QString::fromStdString(s.name);
        if (name.trimmed().isEmpty()) problems << QStringLiteral("A state has no name.");
        else if (states.contains(name)) problems << QStringLiteral("Two states are called \"%1\".").arg(name);
        states.insert(name);
        const QString clip = QString::fromStdString(s.clip);
        if (s.blend == ukiyo::BlendKind::None) {
            if (!clip.isEmpty() && !clipNames.contains(clip))
                problems << QStringLiteral("State \"%1\" plays \"%2\", which is not in the library.").arg(name, clip);
            continue;
        }
        // A blend tree: its parameters must be floats, it needs motions, and each motion needs a library clip.
        // Direct blends take one float parameter per motion instead of X and Y.
        const bool direct = s.blend == ukiyo::BlendKind::Direct;
        const bool twoD = s.blend != ukiyo::BlendKind::Linear1D && !direct;
        const QString px = QString::fromStdString(s.paramX);
        const QString py = QString::fromStdString(s.paramY);
        if (!direct && (px.isEmpty() || !params.contains(px)))
            problems << QStringLiteral("Blend state \"%1\" needs a parameter for X.").arg(name);
        else if (!direct && !floats.contains(px))
            problems << QStringLiteral("Blend state \"%1\" uses \"%2\" for X, which is not a float parameter.").arg(name, px);
        if (twoD) {
            if (py.isEmpty() || !params.contains(py))
                problems << QStringLiteral("Blend state \"%1\" needs a parameter for Y.").arg(name);
            else if (!floats.contains(py))
                problems << QStringLiteral("Blend state \"%1\" uses \"%2\" for Y, which is not a float parameter.").arg(name, py);
        }
        if (s.motions.empty()) problems << QStringLiteral("Blend state \"%1\" has no motions. Add one.").arg(name);
        if (s.blend == ukiyo::BlendKind::FreeformDirectional2D && !s.motions.empty()) {
            // Unity's freeform directional rule: one motion sits at (0, 0), the idle pose.
            bool hasIdle = false;
            for (const ukiyo::BlendMotion& m : s.motions)
                if (m.x == 0.0f && m.y == 0.0f) hasIdle = true;
            if (!hasIdle)
                problems << QStringLiteral("Blend state \"%1\" needs a motion at (0, 0), the idle pose.").arg(name);
        }
        for (size_t k = 0; k < s.motions.size(); ++k) {
            const QString mc = QString::fromStdString(s.motions[k].clip);
            if (mc.isEmpty() || !clipNames.contains(mc))
                problems << QStringLiteral("Blend state \"%1\", motion %2, uses a clip that is not in the library.")
                                .arg(name).arg(static_cast<int>(k) + 1);
            if (s.motions[k].timeScale <= 0.0f)
                problems << QStringLiteral("Blend state \"%1\", motion %2, needs a time scale above 0.")
                                .arg(name).arg(static_cast<int>(k) + 1);
            if (direct) {
                const QString mp = QString::fromStdString(s.motions[k].param);
                if (mp.isEmpty() || !params.contains(mp))
                    problems << QStringLiteral("Blend state \"%1\", motion %2, needs a parameter for its weight.")
                                    .arg(name).arg(static_cast<int>(k) + 1);
                else if (!floats.contains(mp))
                    problems << QStringLiteral("Blend state \"%1\", motion %2, uses \"%3\", which is not a float parameter.")
                                    .arg(name).arg(static_cast<int>(k) + 1).arg(mp);
            }
        }
    }
    if (!states.contains(QString::fromStdString(a.defaultState)))
        problems << QStringLiteral("Choose a default state.");
    for (const ukiyo::AnimTransitionDef& t : a.transitions) {
        const QString from = QString::fromStdString(t.from);
        const QString to = QString::fromStdString(t.to);
        const QString label = QStringLiteral("%1 to %2").arg(from, to);
        if (!states.contains(to)) problems << QStringLiteral("A transition (%1) goes to an unknown state.").arg(label);
        if (from != QLatin1String("*") && !states.contains(from))
            problems << QStringLiteral("A transition (%1) starts from an unknown state.").arg(label);
        if (from == to) problems << QStringLiteral("A transition must join two different states (%1).").arg(label);
        for (const ukiyo::AnimStateMachine::Condition& c : t.when) {
            if (!params.contains(QString::fromStdString(c.param)))
                problems << QStringLiteral("A condition (%1) uses parameter \"%2\", which is not declared.")
                                .arg(label, QString::fromStdString(c.param));
        }
        if (t.when.empty())
            problems << QStringLiteral("Transition %1 has no conditions, so it fires at once.").arg(label);
    }
    return problems;
}

} // namespace animfile
