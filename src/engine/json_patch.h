// Undo as deltas, not documents.
//
// The editor's undo stack used to hold one full copy of the scene per edit. That is correct and it is what most editors
// start with, but the cost is the whole document per keystroke, and a scene with a terrain tile in it is a large
// document. A JsonPatch is the difference between two documents: the ops that turn the old one into the new one. Storing
// the inverse of each edit makes undo a replay, and costs only what changed.
//
// Paths are RFC 6901 JSON Pointers ("/models/3/position/1"), which is also what the scene files use, so a patch in a log
// says exactly which instance and which field an edit touched.
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace ukiyo {

using json = nlohmann::json;

struct JsonPatchOp {
    enum class Kind { Add, Remove };
    Kind kind = Kind::Add;
    std::string path;   // JSON Pointer
    json value;         // for Add
};

using JsonPatch = std::vector<JsonPatchOp>;

// The edit that turns `before` into `after`. Equal subtrees produce no ops, an object key that changed produces one op,
// and an array that changed length is replaced whole (that is what a scene's instance list is). The empty patch means
// the documents are equal.
JsonPatch diffJson(const json& before, const json& after);

// Applies a patch in order. Returns false with a reason when a path does not resolve — a patch built against a
// different document, which is a bug in the caller, not in the scene.
bool applyJsonPatch(json* doc, const JsonPatch& patch, std::string* error);

// The patch that undoes `patch` when it was applied to `before`. Removing needs the old value, so `before` is required.
JsonPatch invertJsonPatch(const JsonPatch& patch, const json& before);

// Escapes a key for a pointer ("a/b" -> "a~1b", "a~b" -> "a~0b"). Exposed for tests and for callers that build paths.
std::string jsonPointerEscape(const std::string& key);

} // namespace ukiyo
