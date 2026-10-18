#!/usr/bin/env python3
"""Compiles a graph per node so every node template is exercised and type checked.

  python3 tools/sg_coverage_check.py            # every node, every master, plus the shipped library
  python3 tools/sg_coverage_check.py --unused   # also list node types no shipped shader uses

ukiyo_shader_tool has a `coverage` mode that builds one graph using every node and compiles it with
glslang. This is the same idea without Qt: one small graph per node, compiled by tools/sg_mirror.py and
type checked by tools/sg_glsl_check.py, so a template that does not compile is caught here rather than
at engine start-up. Per-node graphs also name the node that broke.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import sg_build  # noqa: E402
import sg_glsl_check  # noqa: E402
import sg_mirror  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SHADER_DIR = os.path.join(ROOT, "assets", "Shaders")

# A constant node per port kind: what an unconnected input falls back to anyway.
CONSTANT = sg_build.CONSTANT


def node_graph(spec):
    """One graph where `spec` feeds a PBR Master, directly or through a texture sample."""
    b = sg_build.Builder()
    nid = b.node(spec.id, sg_mirror.default_params(spec.param) if spec.param != "texture" else [""]).id
    b.fill_inputs(nid)
    for out_index, port in enumerate(spec.outputs):
        if port.kind == "float":
            b.wire(b.nodes[nid] and sg_build.Ref(b, nid, out_index), b.master("PBRMaster"), 1)   # Metallic
            break
        if port.kind == "vec3":
            b.wire(sg_build.Ref(b, nid, out_index), b.master("PBRMaster"), 0)                    # Base Color
            break
        if port.kind == "vec2":
            tex = b.texture("")
            b.wire(sg_build.Ref(b, nid, out_index), tex.id, 0)
            b.wire(tex, b.master("PBRMaster"), 0)
            break
    return b.graph()


def master_graph(spec):
    """A master with every input wired, and the outline trio exposed when it is a toon master."""
    b = sg_build.Builder()
    b.fill_inputs(b.master(spec.id))
    return b.graph()


def outline_graph():
    """The three exposed outline values, which ask the engine for the inverted-hull pass."""
    b = sg_build.Builder()
    b.fill_inputs(b.master("ToonMaster"))
    b.color((0.1, 0.08, 0.12), "outlineColor")
    b.float_(0.02, "outlineWidth")
    b.float_(1.0, "outlineAlpha")
    return b.graph()


def compile_and_check(graph, label):
    """Returns a list of problems: generator errors, then GLSL problems."""
    doc = sg_mirror.shader_document(graph)
    compiled = doc["compiled"]
    problems = [f"{label}: {e}" for e in compiled["errors"]]
    if problems:
        return problems, doc
    problems.extend(f"{label}: {p}" for p in sg_glsl_check.check_fragment(compiled["fragment"], label))
    return problems, doc


def shipped_coverage():
    """Which node types the shipped library actually uses."""
    used = {}
    for name in sorted(os.listdir(SHADER_DIR)):
        if not name.endswith(".ukshader"):
            continue
        with open(os.path.join(SHADER_DIR, name), "r", encoding="utf-8") as f:
            doc = json.load(f)
        for node in doc.get("graph", {}).get("nodes", []):
            model = node.get("internal-data", {}).get("model-name", "")
            used.setdefault(model, []).append(name[:-9])
    return used


def main(argv):
    problems = []
    checked = 0

    for spec in sg_mirror.NODES:
        if spec.master:
            continue
        graph = node_graph(spec)
        found, _ = compile_and_check(graph, spec.id)
        problems.extend(found)
        checked += 1
        # The same node with nothing connected: the port defaults must be valid GLSL too.
        empty = sg_build.Builder()
        lone = sg_build.Ref(empty, empty.node(
            spec.id, sg_mirror.default_params(spec.param) if spec.param != "texture" else [""]).id, 0)
        kind = spec.outputs[0].kind if spec.outputs else "float"
        if kind == "vec2":
            tex = empty.texture("")
            empty.wire(lone, tex.id, 0)
            empty.master("PBRMaster", base_color=tex)
        else:
            empty.master("PBRMaster", **({"base_color": lone} if kind == "vec3" else {"metallic": lone}))
        found, _ = compile_and_check(empty.graph(), f"{spec.id} (defaults)")
        problems.extend(found)
        checked += 1

    for spec in sg_mirror.NODES:
        if not spec.master:
            continue
        found, _ = compile_and_check(master_graph(spec), spec.id)
        problems.extend(found)
        checked += 1

    found, doc = compile_and_check(outline_graph(), "outline trio")
    problems.extend(found)
    checked += 1
    if not doc["compiled"].get("outline"):
        problems.append("outline trio: the document does not ask for the outline pass")
    names = [p["name"] for p in doc["compiled"]["properties"]]
    if names[:3] != ["outlineColor", "outlineWidth", "outlineAlpha"]:
        problems.append(f"outline trio: properties start {names[:3]}, the loader needs the outline three")

    used = shipped_coverage()
    unused = [s.id for s in sg_mirror.NODES if s.id not in used]
    print(f"compiled and type checked {checked} graphs over {len(sg_mirror.NODES)} node types")
    print(f"the shipped library uses {len(used)} node types")
    if unused:
        print("never used by a shipped shader: " + ", ".join(unused))
    if "--unused" in argv:
        for model in sorted(used):
            print(f"  {model}: {len(used[model])} shaders")
    for p in problems:
        print("  " + p)
    print(f"{len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
