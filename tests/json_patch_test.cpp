// Undo deltas: the patch between two documents, applying it, and the inverse that undoes it.
//
// The editor's undo stack stores these instead of whole scenes (editor/src/main_window.cpp, beginEdit/undoEdit). The
// properties that matter are the ones a stack needs: an unchanged document produces no patch, applying a patch equals
// the new document, and undoing every patch in reverse order lands exactly on the original — including removals and
// list edits.
#include "engine/json_patch.h"

#include <cstdio>
#include <string>

using namespace ukiyo;

namespace {
int checks = 0;
int failures = 0;
void check(bool ok, const char* what, const std::string& detail = std::string()) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s%s%s\n", what, detail.empty() ? "" : " -- ", detail.c_str());
    }
}

json sceneDoc() {
    return json::parse(R"({
      "format": "ukiyo-scene",
      "models": [
        {"name": "Court", "path": "assets/platform.glb", "position": [0, 0, 0]},
        {"name": "Hero", "path": "assets/hero.vrm", "position": [0, 0, 3.4], "rotation": [0, 180, 0]},
        {"name": "Lantern", "path": "assets/paper_lantern.glb", "position": [3.45, 1.15, 0]}
      ],
      "lights": [{"type": "point", "intensity": 3.2}],
      "settings": {"bloom": true, "exposure": 1.05}
    })");
}
} // namespace

int main() {
    // --- Equal documents make an empty patch ---------------------------------------------------------
    {
        const json a = sceneDoc();
        check(diffJson(a, a).empty(), "an unchanged document produces no ops");
    }

    // --- One field: exactly one op, and it names the field -------------------------------------------
    {
        json before = sceneDoc();
        json after = before;
        after["models"][1]["position"][1] = 1.35;
        const JsonPatch p = diffJson(before, after);
        check(p.size() == 1, "moving one instance is one op", std::to_string(p.size()));
        if (p.size() == 1) {
            check(p[0].path == "/models/1/position/1", "the op names the instance's component", p[0].path);
            check(p[0].value == json(1.35), "the op carries the new value");
        }
        json doc = before;
        std::string err;
        check(applyJsonPatch(&doc, p, &err), "the patch applies", err);
        check(doc == after, "applying the patch reaches the new document");
    }

    // --- Undo: applying the inverse lands exactly on the original ------------------------------------
    {
        json before = sceneDoc();
        json after = before;
        after["models"][1]["position"][1] = 1.35;
        after["settings"]["exposure"] = 1.25;
        after["models"].erase(2);                       // a deletion
        after["models"][0]["name"] = "Court (renamed)";
        after["newKey"] = json::array({1, 2, 3});       // an addition
        json doc = before;
        std::string err;
        const JsonPatch forward = diffJson(before, after);
        check(applyJsonPatch(&doc, forward, &err), "a mixed patch applies", err);
        check(doc == after, "a mixed patch reaches the new document");
        const JsonPatch back = invertJsonPatch(forward, before);
        check(applyJsonPatch(&doc, back, &err), "the inverse applies", err);
        check(doc == before, "undo reaches the original document exactly");
    }

    // --- A whole array replaced (an instance added in the middle) -------------------------------------
    {
        json before = sceneDoc();
        json after = before;
        after["models"].insert(after["models"].begin() + 1, json{{"name", "Inserted"}, {"path", "a.glb"}});
        const JsonPatch p = diffJson(before, after);
        check(p.size() == 1 && p[0].path == "/models", "a list that changed length is one replacement");
        json doc = before;
        std::string err;
        check(applyJsonPatch(&doc, p, &err) && doc == after, "the replacement applies", err);
        check(applyJsonPatch(&doc, invertJsonPatch(p, before), &err) && doc == before, "and undoes", err);
    }

    // --- Stack semantics: three edits, undone in reverse ----------------------------------------------
    {
        json d0 = sceneDoc();
        json d1 = d0; d1["settings"]["exposure"] = 1.1;
        json d2 = d1; d2["models"][0]["rotation"] = json::array({0, 90, 0});
        json d3 = d2; d3["lights"][0]["intensity"] = 4.0;

        JsonPatch p1 = diffJson(d0, d1);
        JsonPatch p2 = diffJson(d1, d2);
        JsonPatch p3 = diffJson(d2, d3);
        std::string err;
        json doc = d0;
        check(applyJsonPatch(&doc, p1, &err) && applyJsonPatch(&doc, p2, &err) && applyJsonPatch(&doc, p3, &err),
              "three edits apply in order", err);
        check(doc == d3, "the document is the third edit");
        // Each stack entry keeps the inverse it needs; the document it was taken from is not retained.
        check(applyJsonPatch(&doc, invertJsonPatch(p3, d2), &err), "undo 3", err);
        check(applyJsonPatch(&doc, invertJsonPatch(p2, d1), &err), "undo 2", err);
        check(applyJsonPatch(&doc, invertJsonPatch(p1, d0), &err), "undo 1", err);
        check(doc == d0, "undoing the stack in reverse reaches the start");
    }

    // --- Keys that need escaping ---------------------------------------------------------------------
    {
        check(jsonPointerEscape("a/b") == "a~1b", "a slash is escaped");
        check(jsonPointerEscape("a~b") == "a~0b", "a tilde is escaped");
        json before = json::object();
        json after = json::object();
        after["weird/key"] = 1;
        json doc = before;
        std::string err;
        check(applyJsonPatch(&doc, diffJson(before, after), &err) && doc == after, "a key with a slash patches", err);
    }

    // --- A patch against the wrong document fails instead of corrupting it ----------------------------
    {
        json a = sceneDoc();
        json b = a;
        b["models"][0]["position"][2] = 5.0;
        JsonPatch p = diffJson(a, b);
        json other = json::parse(R"({"format": "something-else"})");
        std::string err;
        check(!applyJsonPatch(&other, p, &err), "a patch built for another document is refused");
        check(!err.empty(), "and says why", err);
    }

    std::printf("json_patch_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
