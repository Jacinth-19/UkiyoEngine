// Reading a .ukscene without Qt and without the editor.
//
// The editor owns scene files through Qt's JSON classes, which the runtime cannot use: the player links the engine, not
// the editor. This is the same format read into plain structs, so a build can boot a scene the editor saved. It is also
// the only place outside the editor that knows the format, which keeps the player honest: if a field stops loading here,
// the player test fails rather than a shipped game.
//
// Supported today: models with transforms, the instance hierarchy, systems components, scene lights, camera, and the
// render settings the engine can apply. Prefabs are flattened by the editor when it writes a scene only if the scene was
// authored without them; a prefab reference is reported as a warning and skipped, because flattening a prefab needs the
// editor's prefab walker (a later step).
#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "engine/lights.h"
#include "engine/renderer.h"
#include "engine/transform.h"
#include "systems/components.h"

namespace ukiyo {

struct RuntimeInstance {
    std::string path;        // absolute model path after resolution
    std::string name;        // instance name, or the file name when the scene has none
    Transform transform;     // local transform
    int parent = -1;         // index into RuntimeScene::instances, -1 for a root
    bool empty = false;      // an empty object: no model to load
    std::string prefab;      // non-empty when the entry is a prefab reference (not loaded yet)
    SystemsComponents components;
    std::vector<float> lod;
};

struct RuntimeScene {
    std::string sourcePath;
    std::string projectRoot;
    std::vector<RuntimeInstance> instances;
    std::vector<SceneLight> lights;
    RenderSettings settings;
    // Camera the scene was saved with.
    glm::vec3 cameraTarget{0.0f, 1.0f, 0.0f};
    float cameraDistance = 6.0f;
    float cameraYaw = 0.6f;
    float cameraPitch = 0.15f;
    float cameraFovY = 0.9f;
    std::vector<std::string> warnings;
};

// Reads a scene file. Returns false only when the file cannot be read or is not a Ukiyo scene; per-instance problems
// are warnings and do not fail the load, matching the editor's behaviour.
bool loadRuntimeScene(const std::string& path, const std::string& projectRoot, RuntimeScene* out,
                      std::string* error);

} // namespace ukiyo
