#include "player/player_app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

// Key names. The mapping below is GLFW's numbering because that is what a desktop window reports, but nothing else in
// this file needs GLFW: on Android the codes are supplied here and the names come from the touch controls instead. The
// values are GLFW's published constants, stable across GLFW 3.x.
#if defined(__has_include)
#if __has_include(<GLFW/glfw3.h>)
#include <GLFW/glfw3.h>
#define UKIYO_HAVE_GLFW_KEYS 1
#endif
#endif
#ifndef UKIYO_HAVE_GLFW_KEYS
#define GLFW_KEY_SPACE 32
#define GLFW_KEY_ESCAPE 256
#define GLFW_KEY_ENTER 257
#define GLFW_KEY_TAB 258
#define GLFW_KEY_BACKSPACE 259
#define GLFW_KEY_INSERT 260
#define GLFW_KEY_DELETE 261
#define GLFW_KEY_RIGHT 262
#define GLFW_KEY_LEFT 263
#define GLFW_KEY_DOWN 264
#define GLFW_KEY_UP 265
#define GLFW_KEY_PAGE_UP 266
#define GLFW_KEY_PAGE_DOWN 267
#define GLFW_KEY_HOME 268
#define GLFW_KEY_END 269
#define GLFW_KEY_CAPS_LOCK 280
#define GLFW_KEY_F1 290
#define GLFW_KEY_F25 314
#define GLFW_KEY_LEFT_SHIFT 340
#define GLFW_KEY_LEFT_CONTROL 341
#define GLFW_KEY_LEFT_ALT 342
#define GLFW_KEY_LEFT_SUPER 343
#define GLFW_KEY_RIGHT_SHIFT 344
#define GLFW_KEY_RIGHT_CONTROL 345
#define GLFW_KEY_RIGHT_ALT 346
#define GLFW_KEY_RIGHT_SUPER 347
#endif

#include "systems/anim_state.h"

namespace ukiyo {

namespace {

std::string joinPath(const std::string& root, const std::string& rel) {
    if (rel.empty()) return {};
    if (!rel.empty() && (rel[0] == '/' || (rel.size() > 1 && rel[1] == ':'))) return rel;
    return root + "/" + rel;
}

// The rotation part of an instance's world matrix, in the engine's ZXY degrees. A collider is built from it so a
// tilted prop, or one parented to something tilted, collides the way it is drawn.
glm::vec3 worldRotationOf(const glm::mat4& world) {
    Transform t;
    if (!decompose(world, &t)) return glm::vec3(0.0f);
    return t.rotationDeg;
}

} // namespace

PlayerApp::PlayerApp(bool audio) : audio_(audio) {}
PlayerApp::~PlayerApp() = default;

bool PlayerApp::start(const std::string& buildPath, const std::string& projectRoot, std::string* error) {
    projectRoot_ = projectRoot;
    if (!loadBuildManifest(joinPath(projectRoot, buildPath), &manifest_, error)) return false;
    if (!loadScene(firstScene(manifest_), error)) return false;
    started_ = true;
    quit_ = false;
    return true;
}

void PlayerApp::stop() {
    world_.reset();
    instances_.clear();
    started_ = false;
}

bool PlayerApp::loadScene(const std::string& relativePath, std::string* error) {
    RuntimeScene scene;
    std::string err;
    if (!loadRuntimeScene(joinPath(projectRoot_, relativePath), projectRoot_, &scene, &err)) {
        if (error) *error = err;
        return false;
    }
    scene_ = std::move(scene);
    warnings_ = scene_.warnings;

    // One entry per instance, with the world matrix resolved through the hierarchy.
    instances_.clear();
    instances_.reserve(scene_.instances.size());
    for (const RuntimeInstance& in : scene_.instances) {
        PlayerInstance p;
        p.name = in.name;
        p.modelPath = in.empty ? std::string() : in.path;
        p.local = in.transform;
        p.parent = in.parent;
        p.components = in.components;
        p.position = in.transform.position;
        instances_.push_back(std::move(p));
    }
    std::vector<HierarchyNode> nodes(instances_.size());
    for (size_t i = 0; i < instances_.size(); ++i) {
        nodes[i].parent = instances_[i].parent;
        nodes[i].local = instances_[i].local;
    }
    const std::vector<glm::mat4> world = resolveHierarchy(nodes);
    if (!world.empty())
        for (size_t i = 0; i < instances_.size(); ++i) instances_[i].world = world[i];

    characterIndex_ = -1;
    for (size_t i = 0; i < instances_.size(); ++i)
        if (instances_[i].components.character.enabled) {
            characterIndex_ = static_cast<int>(i);
            break;
        }
    keys_.clear();
    buildWorld();
    ++generation_;
    playTime_ = 0.0;
    return true;
}

void PlayerApp::buildWorld() {
    world_ = std::make_unique<World>(!audio_);
    world_->setProjectRoot(projectRoot_);
    world_->setSaveDir(projectRoot_ + "/saves");
    world_->setSceneLights(scene_.lights);
    lastEntityPos_.assign(instances_.size(), glm::vec3(0.0f));
    speed_.assign(instances_.size(), 0.0f);

    bool anyAgent = false;
    for (const PlayerInstance& in : instances_) anyAgent = anyAgent || in.components.agent.enabled;
    if (anyAgent) {
        // The player bakes from the rest-pose geometry, the same source the editor uses.
        std::vector<glm::vec3> verts;
        std::vector<uint32_t> tris;
        // Models are loaded by the renderer, so the player cannot read their geometry here. A scene whose agents need a
        // navmesh logs the limitation rather than silently standing still.
        warnings_.push_back("navigation needs a baked navmesh; the player does not bake one yet");
    }

    for (size_t i = 0; i < instances_.size(); ++i) {
        PlayerInstance& in = instances_[i];
        const SystemsComponents& c = in.components;
        // Every instance gets an entity, not only the ones with a component: a scene's props are things a script may
        // move, light up or test the distance to, and `ukiyo.setPosition("Lantern 3", …)` has to work on a lantern that
        // has no body. An entity with no component is inert — it costs a map entry and nothing per frame.
        Entity& e = world_->entity(in.name);
        e.position = in.position;
        e.rotationDeg = in.local.rotationDeg;
        if (c.body.enabled) {
            // The collider is turned the way the instance is drawn. A ramp the gizmo tilted about X or Z has to be a
            // ramp to the physics as well, so it takes the world rotation: a child of a tilted parent tilts with it.
            const glm::vec3 worldTurn = worldRotationOf(in.world);
            if (c.body.dynamic) {
                if (c.body.box) world_->attachDynamicBox(in.name, c.body.halfExtents, c.body.mass, worldTurn);
                else world_->attachDynamicSphere(in.name, c.body.radius, c.body.mass);
            } else {
                e.body = world_->physics.addStaticBox(in.position, c.body.box ? c.body.halfExtents : glm::vec3(c.body.radius),
                                                     worldTurn);
            }
        }
        if (c.character.enabled) {
            CharacterSettings cs;
            cs.radius = c.character.radius;
            cs.height = c.character.height;
            cs.maxSlopeDeg = c.character.maxSlopeDeg;
            cs.stepHeight = c.character.stepHeight;
            cs.jumpSpeed = c.character.jumpSpeed;
            world_->attachCharacter(in.name, cs);
        }
        if (c.agent.enabled) world_->attachAgent(in.name, c.agent.maxSpeed);
        if (c.emitter.enabled) world_->vfx.spawn(c.emitter.settings, in.position);
        if (c.action.enabled) {
            EntityAction a;
            a.on = true;
            a.pingPong = c.action.pingPong;
            a.speed = c.action.speed;
            a.axis = c.action.axis;
            a.amplitude = c.action.amplitude;
            a.ease = c.action.ease;
            a.startOffset = c.action.startOffset;
            world_->setAction(in.name, a, in.position, in.local.rotationDeg.y);
        }
        if (c.audio.enabled && c.audio.playOnStart && !c.audio.sound.empty()) {
            const std::string abs = joinPath(projectRoot_, c.audio.sound);
            if (AudioEngine::SoundId id = world_->soundFor(abs)) {
                world_->audio.setPosition(id, in.position);
                world_->audio.setSpatial(id, true);
                world_->audio.setLooping(id, c.audio.loop);
                world_->audio.setVolume(id, c.audio.volume);
                world_->audio.play(id);
            } else {
                warnings_.push_back("could not load sound " + c.audio.sound);
            }
        }
        lastEntityPos_[i] = e.position;
    }
    // Every instance with a script gets one. require() reads from the project's script folders, the same way the
    // editor sets them up.
    std::vector<std::string> scriptRoots{joinPath(projectRoot_, "Scripts"), joinPath(projectRoot_, "assets")};
    world_->setScriptSearchPaths(scriptRoots);
    bool anyScript = false;
    for (const PlayerInstance& in : instances_) {
        if (!in.components.script.enabled || in.components.script.path.empty()) continue;
        std::string err;
        if (!world_->loadScriptFor(in.name, joinPath(projectRoot_, in.components.script.path), &err))
            warnings_.push_back("script " + in.components.script.path + ": " + err);
        else
            anyScript = true;
    }
    if (anyScript) world_->notifySceneLoaded();
    // The first instance is the listener.
    if (!instances_.empty()) world_->entity("listener").position = instances_.front().position;
}

void PlayerApp::setKey(const std::string& key, bool down) {
    if (key.empty()) return;
    if (down) keys_.insert(key);
    else keys_.erase(key);
    if (world_) world_->setKeyHeld(key, down);
}

void PlayerApp::setTouch(int finger, bool down, glm::vec2 position, glm::vec2 delta) {
    if (world_) world_->setTouch(finger, down, position, delta);
}

void PlayerApp::syncInstancesFromWorld() {
    if (!world_) return;
    for (size_t i = 0; i < instances_.size(); ++i) {
        PlayerInstance& in = instances_[i];
        const Entity* e = world_->findEntity(in.name);
        if (!e) continue;
        in.position = e->position;
        // The entity drives the root transform's position and, for anything physics moves, its rotation: a box that
        // tumbles is drawn tumbling. Scale stays as authored.
        in.local.position = e->position;
        in.local.rotationDeg = e->rotationDeg;
    }
    std::vector<HierarchyNode> nodes(instances_.size());
    for (size_t i = 0; i < instances_.size(); ++i) {
        nodes[i].parent = instances_[i].parent;
        nodes[i].local = instances_[i].local;
    }
    const std::vector<glm::mat4> world = resolveHierarchy(nodes);
    if (!world.empty())
        for (size_t i = 0; i < instances_.size(); ++i) instances_[i].world = world[i];
}

void PlayerApp::tick(float dt) {
    if (!started_ || !world_) return;
    dt = std::clamp(dt, 0.0f, 0.1f);
    if (dt <= 0.0f) return;
    playTime_ += dt;

    // Locomotion reads the speed measured on the previous step, as in the editor.
    for (size_t i = 0; i < instances_.size(); ++i)
        if (instances_[i].components.anim.enabled) world_->setParam(instances_[i].name, "speed", speed_[i]);

    driveCharacter(dt);
    world_->tick(dt);

    // A script's loadScene replaces the scene between frames.
    if (world_->sceneSwitchReady()) {
        const std::string path = world_->takeSceneRequest();
        std::string err;
        if (!loadScene(path, &err)) warnings_.push_back("loadScene " + path + ": " + err);
        return;
    }
    for (size_t i = 0; i < instances_.size(); ++i) {
        Entity* e = world_->findEntity(instances_[i].name);
        if (!e) continue;
        const glm::vec3 moved = e->position - lastEntityPos_[i];
        speed_[i] = std::sqrt(moved.x * moved.x + moved.z * moved.z) / dt;
        lastEntityPos_[i] = e->position;
    }
    syncInstancesFromWorld();
    if (!instances_.empty()) world_->entity("listener").position = instances_.front().position;
}

bool PlayerApp::saveGame(const std::string& path, std::string* error) const {
    if (!started_) {
        if (error) *error = "the player is not running";
        return false;
    }
    SaveGame save;
    save.setString("scene", scene_.sourcePath);
    save.setFloat("playTime", playTime_);
    save.setInt("instances", static_cast<int64_t>(instances_.size()));
    for (size_t i = 0; i < instances_.size(); ++i) {
        const std::string prefix = "i" + std::to_string(i) + ".";
        save.setString(prefix + "name", instances_[i].name);
        save.setVec3(prefix + "position", instances_[i].local.position);
        // The full rotation, and the yaw on its own for a save read by a build that only knew yaw.
        save.setVec3(prefix + "rotation", instances_[i].local.rotationDeg);
        save.setFloat(prefix + "yaw", instances_[i].local.rotationDeg.y);
    }
    if (!save.saveToFile(path)) {
        if (error) *error = "could not write " + path;
        return false;
    }
    return true;
}

bool PlayerApp::loadGame(const std::string& path, std::string* error) {
    SaveGame save;
    if (!save.loadFromFile(path)) {
        if (error) *error = "could not read " + path;
        return false;
    }
    std::string savedScene;
    if (!save.getString("scene", &savedScene) || savedScene != scene_.sourcePath) {
        if (error) *error = "the save belongs to another scene";
        return false;
    }
    int64_t count = 0;
    if (!save.getInt("instances", &count) || count != static_cast<int64_t>(instances_.size())) {
        if (error) *error = "the save does not match this scene";
        return false;
    }
    // The whole file is validated before anything is applied, so a rejected save leaves the game exactly as it was.
    std::vector<glm::vec3> positions(instances_.size());
    std::vector<glm::vec3> rotations(instances_.size());
    for (size_t i = 0; i < instances_.size(); ++i) {
        const std::string prefix = "i" + std::to_string(i) + ".";
        std::string name;
        if (!save.getString(prefix + "name", &name) || name != instances_[i].name ||
            !save.getVec3(prefix + "position", &positions[i])) {
            if (error) *error = "the save does not match this scene";
            return false;
        }
        // A save written before an instance could tilt carries a yaw alone; that is still a whole rotation.
        if (!save.getVec3(prefix + "rotation", &rotations[i])) {
            double yaw = 0.0;
            if (!save.getFloat(prefix + "yaw", &yaw)) {
                if (error) *error = "the save does not match this scene";
                return false;
            }
            rotations[i] = glm::vec3(0.0f, static_cast<float>(yaw), 0.0f);
        }
    }
    for (size_t i = 0; i < instances_.size(); ++i) {
        instances_[i].local.position = positions[i];
        instances_[i].local.rotationDeg = rotations[i];
        if (world_) {
            if (Entity* e = world_->findEntity(instances_[i].name)) {
                e->position = positions[i];
                e->rotationDeg = rotations[i];
            }
        }
    }
    std::vector<HierarchyNode> nodes(instances_.size());
    for (size_t i = 0; i < instances_.size(); ++i) {
        nodes[i].parent = instances_[i].parent;
        nodes[i].local = instances_[i].local;
    }
    const std::vector<glm::mat4> world = resolveHierarchy(nodes);
    if (!world.empty())
        for (size_t i = 0; i < instances_.size(); ++i) instances_[i].world = world[i];
    double t = 0.0;
    if (save.getFloat("playTime", &t)) playTime_ = t;
    return true;
}

void PlayerApp::driveCharacter(float dt) {
    if (characterIndex_ < 0 || !world_) return;
    const PlayerInstance& in = instances_[static_cast<size_t>(characterIndex_)];
    const CharacterComponent& c = in.components.character;
    const bool w = keys_.count("W") != 0;
    const bool s = keys_.count("S") != 0;
    const bool d = keys_.count("D") != 0;
    const bool a = keys_.count("A") != 0;
    // Camera-relative movement: yaw 0 looks from +Z at the target, so forward is -Z at yaw 0.
    const float yaw = scene_.cameraYaw;
    const glm::vec3 forward(-std::sin(yaw), 0.0f, -std::cos(yaw));
    const glm::vec3 right(std::cos(yaw), 0.0f, -std::sin(yaw));
    glm::vec3 dir = forward * (w ? 1.0f : 0.0f) - forward * (s ? 1.0f : 0.0f) + right * (d ? 1.0f : 0.0f) -
                    right * (a ? 1.0f : 0.0f);
    const float len = glm::length(dir);
    if (len > 1e-4f) dir = dir / len * c.speed;
    const bool jump = keys_.count("Space") != 0;
    world_->setCharacterVelocity(in.name, dir, jump);
    (void)dt;
}

int PlayerApp::scriptsRunning() const {
    if (!world_) return 0;
    int running = 0;
    for (const ScriptInfo& s : world_->scripts.scripts())
        if (s.loaded && !s.stopped) ++running;
    return running;
}

void PlayerApp::notify(const std::string& message) { notification_ = message; }

void PlayerApp::reportFrame(float dt) {
    if (dt <= 0.0f) return;
    frameMs_ = dt * 1000.0f;
    // An exponential average with a time constant near half a second, so the number is readable.
    const float instant = 1.0f / dt;
    fps_ = fps_ <= 0.0f ? instant : fps_ * 0.92f + instant * 0.08f;
}

std::string PlayerApp::title() const {
    return manifest_.name + " " + manifest_.version;
}

PlayerHudText PlayerApp::hudText(uint32_t drawCalls, uint32_t triangles) const {
    HudText t;
    t.title = title();
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%.0f fps   %.2f ms   %u draws   %.1fk tris", double(fps_), double(frameMs_),
                  drawCalls, double(triangles) / 1000.0);
    t.stats = buf;
    t.hint = "WASD move   drag to orbit   wheel zoom   F5 save   F9 load   F1 hide HUD   Esc quit";
    // The game's status line is the game's (`ukiyo.hud`). It used to be routed through notification_, which is the
    // engine's message slot, so every game's own HUD was drawn as an amber warning at 14 px.
    if (world_) t.game = world_->hudText();
    // The newest message wins: a save result is more useful than a warning that is still true.
    if (!notification_.empty()) t.warning = notification_;
    else if (!warnings_.empty()) t.warning = warnings_.front();
    return t;
}

namespace {

struct KeyName {
    int key;
    const char* name;
};

const KeyName kSpecialKeys[] = {
    {GLFW_KEY_SPACE, "Space"},       {GLFW_KEY_ESCAPE, "Esc"},       {GLFW_KEY_ENTER, "Return"},
    {GLFW_KEY_TAB, "Tab"},           {GLFW_KEY_BACKSPACE, "Backspace"}, {GLFW_KEY_DELETE, "Del"},
    {GLFW_KEY_LEFT, "Left"},         {GLFW_KEY_RIGHT, "Right"},      {GLFW_KEY_UP, "Up"},
    {GLFW_KEY_DOWN, "Down"},         {GLFW_KEY_HOME, "Home"},        {GLFW_KEY_END, "End"},
    {GLFW_KEY_PAGE_UP, "PgUp"},      {GLFW_KEY_PAGE_DOWN, "PgDown"}, {GLFW_KEY_INSERT, "Ins"},
    {GLFW_KEY_LEFT_SHIFT, "Shift"},  {GLFW_KEY_RIGHT_SHIFT, "Shift"}, {GLFW_KEY_LEFT_CONTROL, "Ctrl"},
    {GLFW_KEY_RIGHT_CONTROL, "Ctrl"}, {GLFW_KEY_LEFT_ALT, "Alt"},    {GLFW_KEY_RIGHT_ALT, "Alt"},
    {GLFW_KEY_LEFT_SUPER, "Meta"},   {GLFW_KEY_RIGHT_SUPER, "Meta"}, {GLFW_KEY_CAPS_LOCK, "CapsLock"},
};

} // namespace

std::string glfwKeyNameToQt(int glfwKey, const char* printableName) {
    for (const KeyName& k : kSpecialKeys)
        if (k.key == glfwKey) return k.name;
    if (glfwKey >= GLFW_KEY_F1 && glfwKey <= GLFW_KEY_F25) return "F" + std::to_string(glfwKey - GLFW_KEY_F1 + 1);
    if (printableName && printableName[0]) {
        std::string s = printableName;
        if (s.size() == 1 && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
        return s;
    }
    return {};
}

} // namespace ukiyo
