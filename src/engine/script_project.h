// The files a Ukiyo project needs so a Lua editor understands it: the API stub the language server reads, the language
// server's own configuration, a formatter and linter config, and the list of extensions an editor should recommend.
//
// This is pure C++ with no editor and no device, so tests/script_test.cpp checks that every function the engine binds is
// described here, and that the stub stays valid Lua.
#pragma once

#include <string>
#include <vector>

namespace ukiyo::script_project {

// One entry of the scripting API, as documentation and as the stub's declaration.
struct ApiDoc {
    const char* name;       // the field under ukiyo.*, or a class name for the entries that describe a type
    const char* signature;  // the Lua signature, without the "function" keyword
    const char* doc;        // one line
};

const std::vector<ApiDoc>& apiDocs();

// Scripts/.ukiyo/ukiyo.lua: a LuaCATS stub (---@meta) with every entry above, the Ukiyo.Vector3 / Ukiyo.Quaternion /
// Ukiyo.Entity classes and the lifecycle hooks the engine calls. Editors do not run it.
std::string apiStub();

// The files writeSupportFiles() writes, relative to the project root, in a stable order.
const std::vector<std::string>& supportFiles();

// Writes .luarc.json, .editorconfig, stylua.toml, selene.toml, .vscode/extensions.json, .vscode/settings.json and the
// stub under Scripts/.ukiyo/lua/. The editor's own configuration files are only written when they are missing, so a
// project that has customised them keeps them; the stub and .luarc.json are always rewritten (the API moves).
// `written` receives the files that were actually written (relative paths).
bool writeSupportFiles(const std::string& projectRoot, std::vector<std::string>* written, std::string* error);

} // namespace ukiyo::script_project
