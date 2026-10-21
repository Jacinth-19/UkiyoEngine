#include "engine/scene_runtime.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

#include <nlohmann/json.hpp>

namespace ukiyo {

namespace {
using nlohmann::json;

glm::vec3 vec3Of(const json& j, const glm::vec3& fallback) {
    if (!j.is_array() || j.size() < 3) return fallback;
    return glm::vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
}

glm::vec4 vec4Of(const json& j, const glm::vec4& fallback) {
    if (!j.is_array() || j.size() < 4) return fallback;
    return glm::vec4(j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>());
}

std::string resolve(const std::string& root, const std::string& rel) {
    if (rel.empty()) return {};
    if (!rel.empty() && (rel[0] == '/' || (rel.size() > 1 && rel[1] == ':'))) return rel;
    return root + "/" + rel;
}

void readComponents(const json& c, SystemsComponents* out) {
    if (!c.is_object()) return;
    if (c.contains("body") && c["body"].is_object()) {
        const json& b = c["body"];
        out->body.enabled = true;
        out->body.dynamic = b.value("dynamic", true);
        out->body.box = b.value("box", false);
        out->body.radius = b.value("radius", 0.5f);
        out->body.mass = b.value("mass", 1.0f);
        out->body.halfExtents = vec3Of(b.value("halfExtents", json::array()), glm::vec3(0.5f));
    }
    if (c.contains("character") && c["character"].is_object()) {
        const json& ch = c["character"];
        out->character.enabled = true;
        out->character.radius = ch.value("radius", out->character.radius);
        out->character.height = ch.value("height", out->character.height);
        out->character.maxSlopeDeg = ch.value("maxSlope", out->character.maxSlopeDeg);
        out->character.stepHeight = ch.value("stepHeight", out->character.stepHeight);
        out->character.jumpSpeed = ch.value("jump", out->character.jumpSpeed);
        out->character.speed = ch.value("speed", out->character.speed);
    }
    if (c.contains("agent") && c["agent"].is_object()) {
        out->agent.enabled = true;
        out->agent.maxSpeed = c["agent"].value("maxSpeed", 3.5f);
    }
    if (c.contains("audio") && c["audio"].is_object()) {
        const json& a = c["audio"];
        out->audio.enabled = true;
        out->audio.sound = a.value("sound", std::string());
        out->audio.loop = a.value("loop", true);
        out->audio.playOnStart = a.value("playOnStart", true);
        out->audio.volume = a.value("volume", 1.0f);
    }
    if (c.contains("anim") && c["anim"].is_object()) {
        const json& a = c["anim"];
        out->anim.enabled = true;
        out->anim.idleClip = a.value("idle", std::string());
        out->anim.walkClip = a.value("walk", std::string());
        out->anim.runClip = a.value("run", std::string());
        out->anim.walkSpeed = a.value("walkSpeed", 1.6f);
        out->anim.runSpeed = a.value("runSpeed", 5.0f);
    }
    if (c.contains("script") && c["script"].is_object()) {
        out->script.enabled = true;
        out->script.path = c["script"].value("path", std::string());
    }
    if (c.contains("emitter") && c["emitter"].is_object()) {
        const json& e = c["emitter"];
        out->emitter.enabled = true;
        EmitterSettings& s = out->emitter.settings;
        s.rate = e.value("rate", s.rate);
        s.loop = e.value("loop", s.loop);
        s.maxParticles = e.value("maxParticles", s.maxParticles);
        s.lifetime = e.value("lifetime", s.lifetime);
        s.velocityMin = vec3Of(e.value("velocityMin", json::array()), s.velocityMin);
        s.velocityMax = vec3Of(e.value("velocityMax", json::array()), s.velocityMax);
        s.gravity = vec3Of(e.value("gravity", json::array()), s.gravity);
        s.sizeStart = e.value("sizeStart", s.sizeStart);
        s.sizeEnd = e.value("sizeEnd", s.sizeEnd);
        s.colorStart = vec4Of(e.value("colorStart", json::array()), s.colorStart);
        s.colorEnd = vec4Of(e.value("colorEnd", json::array()), s.colorEnd);
    }
    if (c.contains("action") && c["action"].is_object()) {
        const json& a = c["action"];
        out->action.enabled = true;
        out->action.pingPong = a.value("kind", std::string("spin")) == "pingpong";
        out->action.speed = a.value("speed", out->action.speed);
        out->action.axis = vec3Of(a.value("axis", json::array()), out->action.axis);
        out->action.amplitude = a.value("amplitude", out->action.amplitude);
        out->action.ease = a.value("ease", out->action.ease);
        out->action.startOffset = a.value("startOffset", out->action.startOffset);
    }
}

void readSettings(const json& s, RenderSettings* r) {
    if (!s.is_object()) return;
    auto f = [&](const char* key, float& dst) {
        if (s.contains(key) && s[key].is_number()) dst = s[key].get<float>();
    };
    auto b = [&](const char* key, bool& dst) {
        if (s.contains(key) && s[key].is_boolean()) dst = s[key].get<bool>();
    };
    auto i = [&](const char* key, int& dst) {
        if (s.contains(key) && s[key].is_number_integer()) dst = s[key].get<int>();
    };
    b("shadows", r->shadows);
    b("pcss", r->pcss);
    f("pcssSunTanRadius", r->pcssSunTanRadius);
    f("pcssSearchRadius", r->pcssSearchRadius);
    f("pcssMaxPenumbra", r->pcssMaxPenumbra);
    b("vgeom", r->vgeom);
    b("sky", r->sky);
    b("grid", r->grid);
    b("outlines", r->outlines);
    b("bloom", r->bloom);
    i("aaMode", r->aaMode);
    i("tonemap", r->tonemap);
    i("look", r->look);
    i("stylize", r->stylize);
    f("stylizeStrength", r->stylizeStrength);
    f("exposure", r->exposure);
    f("bloomStrength", r->bloomStrength);
    f("bloomThreshold", r->bloomThreshold);
    f("saturation", r->saturation);
    f("contrast", r->contrast);
    f("vignette", r->vignette);
    f("grain", r->grain);
    f("sunIntensity", r->sunIntensity);
    f("ssao", r->ssao);
    f("gi", r->gi);
    f("ssr", r->ssr);
    f("contactShadows", r->contactShadows);
    f("motionBlur", r->motionBlur);
    f("chromatic", r->chromatic);
    f("sharpen", r->sharpen);
    f("outlineScale", r->outlineScale);
    f("fogDensity", r->fogDensity);
    b("celBand", r->celBand);
    if (s.contains("sunDir")) r->sunDir = vec3Of(s["sunDir"], r->sunDir);
}

// The scene's lights (item 6). Values are clamped to what the renderer accepts, exactly as the editor's reader does
// (editor/src/scene_file.cpp), because a shipped game and the editor must shade the same file the same way. A scene
// has one sun, so a second directional light is read as a point light rather than silently dropped.
void readLights(const json& arr, std::vector<SceneLight>* out) {
    if (!arr.is_array()) return;
    int directional = 0;
    for (const json& j : arr) {
        if (!j.is_object()) continue;
        SceneLight l;
        const std::string type = j.value("type", std::string("point"));
        l.type = type == "directional" ? LightType::Directional : type == "spot" ? LightType::Spot : LightType::Point;
        if (l.type == LightType::Directional && ++directional > 1) l.type = LightType::Point;
        l.position = vec3Of(j.value("position", json::array()), l.position);
        l.direction = vec3Of(j.value("direction", json::array()), l.direction);
        l.color = vec3Of(j.value("color", json::array()), l.color);
        l.intensity = std::clamp(j.value("intensity", l.intensity), 0.0f, 64.0f);
        l.radius = std::clamp(j.value("radius", l.radius), 0.05f, 200.0f);
        l.spotAngleDeg = std::clamp(j.value("angle", l.spotAngleDeg), 1.0f, 179.0f);
        l.spotSoftness = std::clamp(j.value("softness", l.spotSoftness), 0.0f, 0.95f);
        l.enabled = j.value("enabled", true);
        out->push_back(l);
    }
}

} // namespace

bool loadRuntimeScene(const std::string& path, const std::string& projectRoot, RuntimeScene* out,
                      std::string* error) {
    if (!out) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "could not open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    json root;
    try {
        root = json::parse(ss.str());
    } catch (const std::exception& e) {
        if (error) *error = path + ": " + e.what();
        return false;
    }
    if (!root.is_object() || root.value("format", std::string()) != "ukiyo-scene") {
        if (error) *error = path + " is not a Ukiyo scene";
        return false;
    }

    RuntimeScene scene;
    scene.sourcePath = path;
    scene.projectRoot = projectRoot;
    readSettings(root.value("settings", json::object()), &scene.settings);
    readLights(root.value("lights", json::array()), &scene.lights);

    const json camera = root.value("gameCamera", json::object());
    scene.cameraTarget = vec3Of(camera.value("target", json::array()), scene.cameraTarget);
    scene.cameraDistance = camera.value("distance", scene.cameraDistance);
    scene.cameraYaw = camera.value("yaw", scene.cameraYaw);
    scene.cameraPitch = camera.value("pitch", scene.cameraPitch);
    scene.cameraFovY = camera.value("fovY", scene.cameraFovY);

    // A legacy single-model scene: {model: "path"} with no instance list.
    json models = root.value("models", json::array());
    if ((!models.is_array() || models.empty()) && root.contains("model") && root["model"].is_string()) {
        json m = json::object();
        m["path"] = root["model"];
        models = json::array({m});
    }
    if (!models.is_array()) {
        if (error) *error = path + ": models is not an array";
        return false;
    }

    std::map<std::string, int> byName;
    int index = 0;
    for (const json& m : models) {
        if (!m.is_object()) continue;
        RuntimeInstance in;
        in.empty = m.value("empty", false);
        in.prefab = m.value("prefab", std::string());
        in.path = resolve(projectRoot, m.value("path", std::string()));
        in.name = m.value("name", std::string());
        if (in.name.empty()) {
            const size_t slash = in.path.find_last_of("/\\");
            in.name = slash == std::string::npos ? in.path : in.path.substr(slash + 1);
        }
        // Position and yaw are the old form; rotation and scale the new one. A file may carry both, and then the full
        // rotation wins for yaw because it is the more specific field.
        if (m.contains("position")) in.transform.position = vec3Of(m["position"], in.transform.position);
        if (m.contains("yawDeg") && m["yawDeg"].is_number()) in.transform.rotationDeg.y = m["yawDeg"].get<float>();
        if (m.contains("rotation")) in.transform.rotationDeg = vec3Of(m["rotation"], in.transform.rotationDeg);
        if (m.contains("scale")) in.transform.scale = vec3Of(m["scale"], in.transform.scale);
        readComponents(m.value("components", json::object()), &in.components);
        if (m.contains("lod") && m["lod"].is_array())
            for (const json& v : m["lod"]) in.lod.push_back(v.get<float>());
        if (m.contains("parent")) {
            if (m["parent"].is_number_integer()) in.parent = m["parent"].get<int>();
            else if (m["parent"].is_string()) {
                const std::string pname = m["parent"].get<std::string>();
                auto it = byName.find(pname);
                if (it != byName.end()) {
                    in.parent = it->second;
                } else {
                    scene.warnings.push_back("instance \"" + in.name + "\": parent \"" + pname + "\" comes later or does not exist");
                }
            }
        }
        if (!in.prefab.empty()) {
            scene.warnings.push_back("instance \"" + in.name + "\": prefab references are not flattened by the player yet");
        } else if (in.path.empty() && !in.empty) {
            scene.warnings.push_back("instance \"" + in.name + "\" has no model path");
        }
        byName[in.name] = index;
        scene.instances.push_back(std::move(in));
        ++index;
    }
    if (scene.instances.empty()) {
        if (error) *error = path + " has no instances";
        return false;
    }

    // Repair parent links: a forward reference by name is resolved above; anything still dangling or cyclic becomes a
    // root, the same rule the editor's loader follows.
    std::vector<HierarchyNode> nodes(scene.instances.size());
    for (size_t k = 0; k < scene.instances.size(); ++k) {
        nodes[k].parent = scene.instances[k].parent;
        nodes[k].local = scene.instances[k].transform;
    }
    int cycle = -1;
    if (hasCycle(nodes, &cycle)) {
        scene.warnings.push_back("instance \"" + scene.instances[static_cast<size_t>(cycle)].name +
                                 "\": parent chain loops; the link was dropped");
        for (size_t k = 0; k < scene.instances.size(); ++k) {
            if (scene.instances[k].parent >= 0) {
                std::vector<HierarchyNode> trial = nodes;
                trial[k].parent = -1;
                if (!hasCycle(trial, nullptr)) {
                    nodes = trial;
                    scene.instances[k].parent = -1;
                }
            }
        }
    }
    *out = std::move(scene);
    return true;
}

} // namespace ukiyo
