#include "systems_json.h"

#include <QDir>
#include <QJsonArray>

#include <glm/glm.hpp>

namespace editor::systems_json {

namespace {

QJsonArray floats(std::initializer_list<float> v) {
    QJsonArray a;
    for (float f : v) a.append(double(f));
    return a;
}

// State machine graph for the locomotion component. Names are fixed strings so the file stays readable.
const char* opName(ukiyo::AnimStateMachine::Condition::Op op) {
    using Op = ukiyo::AnimStateMachine::Condition::Op;
    switch (op) {
    case Op::Greater: return "greater";
    case Op::Less: return "less";
    case Op::Equal: return "equal";
    case Op::IsTrue: return "true";
    case Op::IsFalse: return "false";
    case Op::Trigger: return "trigger";
    }
    return "greater";
}

ukiyo::AnimStateMachine::Condition::Op opFrom(const QString& s) {
    using Op = ukiyo::AnimStateMachine::Condition::Op;
    if (s == "less") return Op::Less;
    if (s == "equal") return Op::Equal;
    if (s == "true") return Op::IsTrue;
    if (s == "false") return Op::IsFalse;
    if (s == "trigger") return Op::Trigger;
    return Op::Greater;
}

QJsonObject animGraphJson(const ukiyo::AnimComponent& a) {
    QJsonObject g;
    g["default"] = QString::fromStdString(a.defaultState);
    QJsonArray params;
    for (const ukiyo::AnimParamDef& p : a.params) {
        QJsonObject o;
        o["name"] = QString::fromStdString(p.name);
        o["type"] = p.type == ukiyo::AnimParamDef::Type::Bool      ? QStringLiteral("bool")
                    : p.type == ukiyo::AnimParamDef::Type::Trigger ? QStringLiteral("trigger")
                                                                   : QStringLiteral("float");
        o["value"] = double(p.value);
        params.append(o);
    }
    g["params"] = params;
    QJsonArray states;
    for (const ukiyo::AnimStateDef& s : a.states) {
        QJsonObject o;
        o["name"] = QString::fromStdString(s.name);
        o["clip"] = QString::fromStdString(s.clip);
        o["x"] = double(s.x);
        o["y"] = double(s.y);
        if (s.blend != ukiyo::BlendKind::None) {
            o["blend"] = QString::fromLatin1(ukiyo::blendKindName(s.blend));
            o["paramX"] = QString::fromStdString(s.paramX);
            if (s.blend != ukiyo::BlendKind::Linear1D) o["paramY"] = QString::fromStdString(s.paramY);
            QJsonArray motions;
            for (const ukiyo::BlendMotion& m : s.motions) {
                QJsonObject mo;
                mo["clip"] = QString::fromStdString(m.clip);
                mo["x"] = double(m.x);
                mo["y"] = double(m.y);
                // Motion options are written only when they differ from the defaults, so older files stay the same.
                if (m.timeScale != 1.0f) mo["timeScale"] = double(m.timeScale);
                if (m.mirror) mo["mirror"] = true;
                if (!m.param.empty()) mo["param"] = QString::fromStdString(m.param);
                motions.append(mo);
            }
            o["motions"] = motions;
        }
        states.append(o);
    }
    g["states"] = states;
    QJsonArray transitions;
    for (const ukiyo::AnimTransitionDef& t : a.transitions) {
        QJsonObject o;
        o["from"] = QString::fromStdString(t.from);
        o["to"] = QString::fromStdString(t.to);
        o["fade"] = double(t.fade);
        o["minDwell"] = double(t.minDwell);
        o["hasExitTime"] = t.opts.hasExitTime;
        o["exitTime"] = double(t.opts.exitTime);
        o["fixedDuration"] = t.opts.fixedDuration;
        o["offset"] = double(t.opts.offset);
        o["toSelf"] = t.opts.toSelf;
        QJsonArray when;
        for (const ukiyo::AnimStateMachine::Condition& c : t.when) {
            QJsonObject w;
            w["param"] = QString::fromStdString(c.param);
            w["op"] = QString::fromLatin1(opName(c.op));
            w["value"] = double(c.value);
            when.append(w);
        }
        o["when"] = when;
        transitions.append(o);
    }
    g["transitions"] = transitions;
    return g;
}

void readAnimGraph(const QJsonObject& g, ukiyo::AnimComponent& a) {
    a.custom = true;
    a.defaultState = g.value("default").toString().toStdString();
    a.params.clear();
    for (const QJsonValue& v : g.value("params").toArray()) {
        const QJsonObject o = v.toObject();
        ukiyo::AnimParamDef p;
        p.name = o.value("name").toString().toStdString();
        const QString type = o.value("type").toString();
        p.type = type == "bool" ? ukiyo::AnimParamDef::Type::Bool
                 : type == "trigger" ? ukiyo::AnimParamDef::Type::Trigger
                                     : ukiyo::AnimParamDef::Type::Float;
        p.value = float(o.value("value").toDouble());
        a.params.push_back(p);
    }
    a.states.clear();
    for (const QJsonValue& v : g.value("states").toArray()) {
        const QJsonObject o = v.toObject();
        ukiyo::AnimStateDef s;
        s.name = o.value("name").toString().toStdString();
        s.clip = o.value("clip").toString().toStdString();
        s.x = float(o.value("x").toDouble());
        s.y = float(o.value("y").toDouble());
        s.blend = ukiyo::blendKindFromName(o.value("blend").toString().toStdString());
        if (s.blend != ukiyo::BlendKind::None) {
            s.paramX = o.value("paramX").toString().toStdString();
            s.paramY = o.value("paramY").toString().toStdString();
            for (const QJsonValue& mv : o.value("motions").toArray()) {
                const QJsonObject mo = mv.toObject();
                ukiyo::BlendMotion m;
                m.clip = mo.value("clip").toString().toStdString();
                m.x = float(mo.value("x").toDouble());
                m.y = float(mo.value("y").toDouble());
                m.timeScale = mo.contains("timeScale") ? float(mo.value("timeScale").toDouble()) : 1.0f;
                m.mirror = mo.value("mirror").toBool(false);
                m.param = mo.value("param").toString().toStdString();
                s.motions.push_back(m);
            }
        }
        a.states.push_back(s);
    }
    a.transitions.clear();
    for (const QJsonValue& v : g.value("transitions").toArray()) {
        const QJsonObject o = v.toObject();
        ukiyo::AnimTransitionDef t;
        t.from = o.value("from").toString().toStdString();
        t.to = o.value("to").toString().toStdString();
        t.fade = float(o.value("fade").toDouble(t.fade));
        t.minDwell = float(o.value("minDwell").toDouble(t.minDwell));
        t.opts.hasExitTime = o.value("hasExitTime").toBool(t.opts.hasExitTime);
        t.opts.exitTime = float(o.value("exitTime").toDouble(t.opts.exitTime));
        t.opts.fixedDuration = o.value("fixedDuration").toBool(t.opts.fixedDuration);
        t.opts.offset = float(o.value("offset").toDouble(t.opts.offset));
        t.opts.toSelf = o.value("toSelf").toBool(t.opts.toSelf);
        for (const QJsonValue& w : o.value("when").toArray()) {
            const QJsonObject c = w.toObject();
            ukiyo::AnimStateMachine::Condition cond;
            cond.param = c.value("param").toString().toStdString();
            cond.op = opFrom(c.value("op").toString());
            cond.value = float(c.value("value").toDouble());
            t.when.push_back(cond);
        }
        a.transitions.push_back(t);
    }
}

// Each reader leaves its value alone unless the array is present and long enough.
void readVec3(const QJsonValue& v, glm::vec3& out) {
    const QJsonArray a = v.toArray();
    if (a.size() >= 3) out = {float(a[0].toDouble()), float(a[1].toDouble()), float(a[2].toDouble())};
}

void readVec4(const QJsonValue& v, glm::vec4& out) {
    const QJsonArray a = v.toArray();
    if (a.size() >= 4)
        out = {float(a[0].toDouble()), float(a[1].toDouble()), float(a[2].toDouble()), float(a[3].toDouble())};
}

QString toRel(const QString& abs, const QString& root) {
    return abs.isEmpty() ? QString() : QDir(root).relativeFilePath(abs);
}

QString toAbs(const QString& rel, const QString& root) {
    return rel.isEmpty() ? QString() : QDir(root).absoluteFilePath(rel);
}

} // namespace

QJsonObject toJson(const ukiyo::SystemsComponents& c, const QString& root) {
    QJsonObject o;
    if (c.body.enabled) {
        QJsonObject b;
        b["dynamic"] = c.body.dynamic;
        b["radius"] = double(c.body.radius);
        b["mass"] = double(c.body.mass);
        b["box"] = c.body.box;
        QJsonArray h;
        for (int k = 0; k < 3; ++k) h.append(double(c.body.halfExtents[k]));
        b["halfExtents"] = h;
        o["body"] = b;
    }
    if (c.character.enabled) {
        QJsonObject ch;
        ch["radius"] = double(c.character.radius);
        ch["height"] = double(c.character.height);
        ch["maxSlope"] = double(c.character.maxSlopeDeg);
        ch["stepHeight"] = double(c.character.stepHeight);
        ch["jump"] = double(c.character.jumpSpeed);
        ch["speed"] = double(c.character.speed);
        o["character"] = ch;
    }
    if (c.agent.enabled) {
        QJsonObject a;
        a["maxSpeed"] = double(c.agent.maxSpeed);
        o["agent"] = a;
    }
    if (c.audio.enabled) {
        QJsonObject a;
        a["sound"] = toRel(QString::fromStdString(c.audio.sound), root);
        a["loop"] = c.audio.loop;
        a["playOnStart"] = c.audio.playOnStart;
        a["volume"] = double(c.audio.volume);
        o["audio"] = a;
    }
    if (c.anim.enabled) {
        QJsonObject a;
        a["idle"] = QString::fromStdString(c.anim.idleClip);
        a["walk"] = QString::fromStdString(c.anim.walkClip);
        a["run"] = QString::fromStdString(c.anim.runClip);
        a["walkSpeed"] = double(c.anim.walkSpeed);
        a["runSpeed"] = double(c.anim.runSpeed);
        if (c.anim.custom) a["graph"] = animGraphJson(c.anim);
        o["anim"] = a;
    }
    if (c.script.enabled) {
        QJsonObject s;
        s["path"] = toRel(QString::fromStdString(c.script.path), root);
        o["script"] = s;
    }
    if (c.emitter.enabled) {
        const ukiyo::EmitterSettings& e = c.emitter.settings;
        QJsonObject p;
        p["rate"] = double(e.rate);
        p["loop"] = e.loop;
        p["maxParticles"] = e.maxParticles;
        p["lifetime"] = double(e.lifetime);
        p["velocityMin"] = floats({e.velocityMin.x, e.velocityMin.y, e.velocityMin.z});
        p["velocityMax"] = floats({e.velocityMax.x, e.velocityMax.y, e.velocityMax.z});
        p["gravity"] = floats({e.gravity.x, e.gravity.y, e.gravity.z});
        p["sizeStart"] = double(e.sizeStart);
        p["sizeEnd"] = double(e.sizeEnd);
        p["colorStart"] = floats({e.colorStart.r, e.colorStart.g, e.colorStart.b, e.colorStart.a});
        p["colorEnd"] = floats({e.colorEnd.r, e.colorEnd.g, e.colorEnd.b, e.colorEnd.a});
        o["emitter"] = p;
    }
    if (c.action.enabled) {
        const ukiyo::ActionComponent& a = c.action;
        QJsonObject act;
        act["kind"] = a.pingPong ? "pingpong" : "spin";
        act["speed"] = double(a.speed);
        act["axis"] = floats({a.axis.x, a.axis.y, a.axis.z});
        act["amplitude"] = double(a.amplitude);
        act["easing"] = a.ease ? "ease" : "linear";
        act["startOffset"] = double(a.startOffset);
        o["action"] = act;
    }
    return o;
}

ukiyo::SystemsComponents fromJson(const QJsonObject& o, const QString& root) {
    ukiyo::SystemsComponents c;
    if (o.contains("body")) {
        const QJsonObject b = o["body"].toObject();
        c.body.enabled = true;
        c.body.dynamic = b.value("dynamic").toBool(true);
        c.body.radius = float(b.value("radius").toDouble(c.body.radius));
        c.body.mass = float(b.value("mass").toDouble(c.body.mass));
        c.body.box = b.value("box").toBool(false);
        const QJsonArray h = b.value("halfExtents").toArray();
        if (h.size() == 3)
            c.body.halfExtents = glm::vec3(float(h[0].toDouble(0.5)), float(h[1].toDouble(0.5)), float(h[2].toDouble(0.5)));
    }
    if (o.contains("character")) {
        const QJsonObject ch = o["character"].toObject();
        c.character.enabled = true;
        c.character.radius = float(ch.value("radius").toDouble(c.character.radius));
        c.character.height = float(ch.value("height").toDouble(c.character.height));
        c.character.maxSlopeDeg = float(ch.value("maxSlope").toDouble(c.character.maxSlopeDeg));
        c.character.stepHeight = float(ch.value("stepHeight").toDouble(c.character.stepHeight));
        c.character.jumpSpeed = float(ch.value("jump").toDouble(c.character.jumpSpeed));
        c.character.speed = float(ch.value("speed").toDouble(c.character.speed));
    }
    if (o.contains("agent")) {
        c.agent.enabled = true;
        c.agent.maxSpeed = float(o["agent"].toObject().value("maxSpeed").toDouble(c.agent.maxSpeed));
    }
    if (o.contains("audio")) {
        const QJsonObject a = o["audio"].toObject();
        c.audio.enabled = true;
        c.audio.sound = toAbs(a.value("sound").toString(), root).toStdString();
        c.audio.volume = float(a.value("volume").toDouble(1.0));
        c.audio.loop = a.value("loop").toBool(true);
        c.audio.playOnStart = a.value("playOnStart").toBool(true);
    }
    if (o.contains("anim")) {
        const QJsonObject a = o["anim"].toObject();
        c.anim.enabled = true;
        c.anim.idleClip = a.value("idle").toString().toStdString();
        c.anim.walkClip = a.value("walk").toString().toStdString();
        c.anim.runClip = a.value("run").toString().toStdString();
        c.anim.walkSpeed = float(a.value("walkSpeed").toDouble(c.anim.walkSpeed));
        c.anim.runSpeed = float(a.value("runSpeed").toDouble(c.anim.runSpeed));
        if (a.contains("graph")) readAnimGraph(a.value("graph").toObject(), c.anim);
    }
    if (o.contains("script")) {
        c.script.enabled = true;
        c.script.path = toAbs(o["script"].toObject().value("path").toString(), root).toStdString();
    }
    if (o.contains("action")) {
        const QJsonObject act = o["action"].toObject();
        ukiyo::ActionComponent& a = c.action;
        a.enabled = true;
        a.pingPong = act.value("kind").toString() == QLatin1String("pingpong");
        a.speed = float(act.value("speed").toDouble(a.speed));
        readVec3(act.value("axis"), a.axis);
        a.amplitude = float(act.value("amplitude").toDouble(a.amplitude));
        a.ease = act.value("easing").toString() == QLatin1String("ease");
        a.startOffset = float(act.value("startOffset").toDouble(a.startOffset));
    }
    if (o.contains("emitter")) {
        const QJsonObject p = o["emitter"].toObject();
        ukiyo::EmitterSettings& e = c.emitter.settings;
        c.emitter.enabled = true;
        e.rate = float(p.value("rate").toDouble(e.rate));
        e.loop = p.value("loop").toBool(e.loop);
        e.maxParticles = p.value("maxParticles").toInt(e.maxParticles);
        e.lifetime = float(p.value("lifetime").toDouble(e.lifetime));
        readVec3(p.value("velocityMin"), e.velocityMin);
        readVec3(p.value("velocityMax"), e.velocityMax);
        readVec3(p.value("gravity"), e.gravity);
        e.sizeStart = float(p.value("sizeStart").toDouble(e.sizeStart));
        e.sizeEnd = float(p.value("sizeEnd").toDouble(e.sizeEnd));
        readVec4(p.value("colorStart"), e.colorStart);
        readVec4(p.value("colorEnd"), e.colorEnd);
    }
    return c;
}

} // namespace editor::systems_json
