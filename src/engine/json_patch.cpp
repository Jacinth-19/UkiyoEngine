#include "engine/json_patch.h"

#include <algorithm>
#include <cstdio>

namespace ukiyo {

namespace {

std::string join(const std::string& base, const std::string& key) { return base + "/" + jsonPointerEscape(key); }
std::string joinIndex(const std::string& base, size_t i) { return base + "/" + std::to_string(i); }

// ~1 is a slash and ~0 a tilde; the order matters, as RFC 6901 says.
std::string unescape(const std::string& token) {
    std::string key;
    key.reserve(token.size());
    for (size_t i = 0; i < token.size(); ++i) {
        if (token[i] == '~' && i + 1 < token.size()) {
            if (token[i + 1] == '1') { key += '/'; ++i; continue; }
            if (token[i + 1] == '0') { key += '~'; ++i; continue; }
        }
        key += token[i];
    }
    return key;
}

// Splits "/a/b" into {"a","b"}. A leading slash is required; "" is the document itself.
std::vector<std::string> tokens(const std::string& path, bool* ok) {
    std::vector<std::string> out;
    *ok = path.empty() || path[0] == '/';
    if (!*ok) return out;
    size_t start = 1;
    while (start <= path.size() && !path.empty()) {
        const size_t slash = path.find('/', start);
        const std::string raw = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (raw.empty() && slash == std::string::npos) break;
        out.push_back(unescape(raw));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return out;
}

// An array step must be a whole number. Anything else is a patch that does not fit the document.
size_t indexOf(const std::string& key, bool* ok) {
    try {
        size_t used = 0;
        const unsigned long long v = std::stoull(key, &used);
        *ok = used == key.size();
        return static_cast<size_t>(v);
    } catch (...) {
        *ok = false;
        return 0;
    }
}

const json* at(const json& doc, const std::vector<std::string>& path) {
    const json* cur = &doc;
    for (const std::string& key : path) {
        if (cur->is_object()) {
            auto it = cur->find(key);
            if (it == cur->end()) return nullptr;
            cur = &*it;
        } else if (cur->is_array()) {
            bool ok = false;
            const size_t idx = indexOf(key, &ok);
            if (!ok || idx >= cur->size()) return nullptr;
            cur = &(*cur)[idx];
        } else {
            return nullptr;
        }
    }
    return cur;
}

json* at(json& doc, const std::vector<std::string>& path) {
    // The const overload does the walking; this one is the same walk with a mutable result.
    return const_cast<json*>(at(static_cast<const json&>(doc), path));
}

void diffInto(const json& a, const json& b, const std::string& path, JsonPatch* out) {
    if (a == b) return;
    if (a.is_object() && b.is_object()) {
        // Removed keys first, then changed and added ones, so a patch reads in document order for the common case of a
        // field being edited.
        for (auto it = a.begin(); it != a.end(); ++it)
            if (!b.contains(it.key())) out->push_back({JsonPatchOp::Kind::Remove, join(path, it.key()), json()});
        for (auto it = b.begin(); it != b.end(); ++it) {
            const std::string child = join(path, it.key());
            auto old = a.find(it.key());
            if (old == a.end()) out->push_back({JsonPatchOp::Kind::Add, child, it.value()});
            else diffInto(*old, it.value(), child, out);
        }
        return;
    }
    if (a.is_array() && b.is_array() && a.size() == b.size()) {
        for (size_t i = 0; i < a.size(); ++i) diffInto(a[i], b[i], joinIndex(path, i), out);
        return;
    }
    // A changed scalar, a changed type, or an array whose length changed: one op replaces it. Inserting into the middle
    // of an array would shift every path after it, and the caller's own document is the reference for the next diff.
    out->push_back({JsonPatchOp::Kind::Add, path, b});
}

} // namespace

std::string jsonPointerEscape(const std::string& key) {
    std::string out;
    out.reserve(key.size());
    for (char c : key) {
        if (c == '~') out += "~0";
        else if (c == '/') out += "~1";
        else out += c;
    }
    return out;
}

JsonPatch diffJson(const json& before, const json& after) {
    JsonPatch patch;
    if (before != after && (!before.is_object() || !after.is_object()) &&
        !(before.is_array() && after.is_array() && before.size() == after.size()))
        patch.push_back({JsonPatchOp::Kind::Add, "", after});
    else
        diffInto(before, after, "", &patch);
    return patch;
}

bool applyJsonPatch(json* doc, const JsonPatch& patch, std::string* error) {
    if (!doc) return false;
    for (const JsonPatchOp& op : patch) {
        bool ok = false;
        const std::vector<std::string> path = tokens(op.path, &ok);
        if (!ok) {
            if (error) *error = "not a JSON pointer: " + op.path;
            return false;
        }
        if (path.empty()) {
            if (op.kind != JsonPatchOp::Kind::Add) {
                if (error) *error = "a remove of the whole document is not a patch";
                return false;
            }
            *doc = op.value;
            continue;
        }
        // Everything but the last token has to exist; the leaf may not, which is what Add means.
        json* container = at(*doc, std::vector<std::string>(path.begin(), path.end() - 1));
        if (!container) {
            if (error) *error = "no such path: " + op.path;
            return false;
        }
        const std::string& key = path.back();
        if (container->is_object()) {
            if (op.kind == JsonPatchOp::Kind::Remove) {
                if (container->erase(key) == 0) {
                    if (error) *error = "no such key: " + op.path;
                    return false;
                }
            } else {
                (*container)[key] = op.value;
            }
        } else if (container->is_array()) {
            bool indexOk = false;
            const size_t idx = indexOf(key, &indexOk);
            if (!indexOk) {
                if (error) *error = "not an array index: " + op.path;
                return false;
            }
            if (op.kind == JsonPatchOp::Kind::Remove) {
                if (idx >= container->size()) {
                    if (error) *error = "index out of range: " + op.path;
                    return false;
                }
                container->erase(container->begin() + static_cast<json::difference_type>(idx));
            } else if (idx < container->size()) {
                (*container)[idx] = op.value;
            } else if (idx == container->size()) {
                container->push_back(op.value);
            } else {
                if (error) *error = "index out of range: " + op.path;
                return false;
            }
        } else {
            if (error) *error = "not a container: " + op.path;
            return false;
        }
    }
    return true;
}

JsonPatch invertJsonPatch(const JsonPatch& patch, const json& before) {
    JsonPatch inverse;
    inverse.reserve(patch.size());
    for (const JsonPatchOp& op : patch) {
        bool ok = false;
        const std::vector<std::string> path = tokens(op.path, &ok);
        const json* old = ok ? at(before, path) : nullptr;
        if (op.kind == JsonPatchOp::Kind::Add) {
            if (old) inverse.push_back({JsonPatchOp::Kind::Add, op.path, *old});
            else inverse.push_back({JsonPatchOp::Kind::Remove, op.path, json()});
        } else if (old) {
            inverse.push_back({JsonPatchOp::Kind::Add, op.path, *old});
        }
    }
    // Undo runs backwards: the last edit is the first thing undone.
    std::reverse(inverse.begin(), inverse.end());
    return inverse;
}

} // namespace ukiyo
