// Project templates and the new-project scaffold used by the Ukiyo Hub. Plain C++ with no Qt, so the
// hub, the editor, and the tests share one implementation.
//
// A project is a folder with:
//   ukiyo.project.json                     manifest: name, template, engine root, main scene
//   ProjectSettings/ProjectSettings.json   the engine's project settings (product name, input, ...)
//   Scenes/Main.ukscene                    a scene the editor opens
//   assets/                                the project's own files (a terrain heightmap, for example)
//
// Templates may also use content that ships with the engine. Those files are not copied into the
// project: the scene stores a path relative to the project folder, and the manifest records the
// engine root, so a template can stay small.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace engine {

struct ProjectTemplate {
    std::string id;            // stable key, used in the manifest
    std::string name;          // "Terrain Lab"
    std::string tagline;       // one line for the hub's template card
    std::string description;   // a paragraph for the template page
    std::vector<std::string> bullets;   // what the template sets up
    std::string accent;        // "#RRGGBB", used for the hub card's accent
};

// The built-in templates, in the order the hub shows them.
const std::vector<ProjectTemplate>& projectTemplates();
const ProjectTemplate* findProjectTemplate(const std::string& id);

struct ProjectManifest {
    std::string name;
    std::string templateId;
    std::string mainScene = "Scenes/Main.ukscene";
    std::string engineRoot;                 // absolute path of the engine checkout; may be empty
    std::string engineVersion = "0.1";
    std::string createdUtc;                 // ISO 8601, for example 2026-10-19T02:20:00Z
};

// The manifest file for a project root: <root>/ukiyo.project.json.
std::string projectManifestPath(const std::string& root);

// A missing or unreadable manifest fails with a reason, so the hub can offer to adopt the folder.
bool loadProjectManifest(const std::string& root, ProjectManifest* out, std::string* error);
bool saveProjectManifest(const std::string& root, const ProjectManifest& manifest, std::string* error);

// True when the folder holds a readable manifest and its main scene.
bool isProject(const std::string& root);

// Called while a project is created: 0..1 progress and the line the splash shows.
using ProjectProgress = std::function<void(float fraction, const std::string& message)>;

struct CreateProjectOptions {
    std::string root;         // folder to create; it must not exist, or must be empty
    std::string name;         // project name, also the product name in ProjectSettings
    std::string templateId;   // from projectTemplates()
    std::string engineRoot;   // engine checkout; template assets are linked relative to the project
};

// Creates the project. On failure the folder this call made is removed again and *error says why.
// `progress` may be empty.
bool createProject(const CreateProjectOptions& options, const ProjectProgress& progress, std::string* error);

// A name that is usable as a project and folder name: letters, digits, spaces, '-', '_' and '.'.
// A name starting with '.' is refused, as are names longer than 64 characters. `why` is optional.
bool validProjectName(const std::string& name, std::string* why = nullptr);

} // namespace engine
