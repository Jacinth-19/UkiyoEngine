#!/usr/bin/env python3
"""Compiles shader-graph sources into .ukshader assets without Qt.

  compile <graphDir> <assetsDir>   every <Name>.json graph -> <assetsDir>/Shaders/<Name>.ukshader
                                   (the same job as `ukiyo_shader_tool compile`, see tools/sg_mirror.py)
  refresh <assetsDir>              re-compile every shipped graph from the graph it already carries
  verify  <assetsDir>              re-compile and fail if any shipped file is not byte identical

`verify` is the regression test for the mirror: it proves the Python generator and the editor's C++
generator agree on every graph in assets/Shaders. Run it after touching sg_mirror.py, sg_nodes.cpp or
sg_codegen.cpp.

Hand-written shaders (EndfieldHybrid, NahidaFace, ...) carry "handWritten": true and an empty graph;
they are skipped, because their GLSL is not generated.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import sg_mirror  # noqa: E402


def load_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def is_generated(doc):
    return not doc.get("handWritten") and len(doc.get("graph", {}).get("nodes", [])) > 0


def compile_graph_json(graph_json, groups=None):
    return sg_mirror.shader_document(graph_json, groups)


def compile_dir(graph_dir, assets_dir):
    out_dir = os.path.join(assets_dir, "Shaders")
    os.makedirs(out_dir, exist_ok=True)
    names = sorted(f for f in os.listdir(graph_dir) if f.endswith(".json"))
    failed = []
    for name in names:
        stem = name[:-5]
        doc = load_json(os.path.join(graph_dir, name))
        result = compile_graph_json(doc.get("graph", {}))
        errors = result["compiled"]["errors"]
        if errors:
            failed.append((stem, errors))
            continue
        path = os.path.join(out_dir, stem + ".ukshader")
        with open(path, "wb") as f:
            f.write(sg_mirror.write_document(result))
    print(f"compiled {len(names) - len(failed)} of {len(names)} graphs into {out_dir}")
    for stem, errors in failed:
        print(f"  FAILED {stem}: " + "; ".join(errors), file=sys.stderr)
    return not failed


def refresh(assets_dir, write=True):
    """Re-compiles every generated shader in place. Returns (count, problems)."""
    shader_dir = os.path.join(assets_dir, "Shaders")
    names = sorted(f for f in os.listdir(shader_dir) if f.endswith(".ukshader"))
    count = 0
    problems = []
    for name in names:
        path = os.path.join(shader_dir, name)
        doc = load_json(path)
        if not is_generated(doc):
            continue
        fresh = compile_graph_json(doc["graph"], doc.get("groups"))
        if fresh["compiled"]["errors"]:
            problems.append((name, "; ".join(fresh["compiled"]["errors"])))
            continue
        # Anything the editor added beyond the generated keys (a note, for example) is kept.
        for key, value in doc.items():
            if key not in fresh:
                fresh[key] = value
        data = sg_mirror.write_document(fresh)
        with open(path, "rb") as f:
            old = f.read()
        if data != old:
            count += 1
            if write:
                with open(path, "wb") as f:
                    f.write(data)
    return count, problems


def verify(assets_dir, verbose=False):
    """Byte-compares every generated shader with a fresh compile of its own graph."""
    shader_dir = os.path.join(assets_dir, "Shaders")
    names = sorted(f for f in os.listdir(shader_dir) if f.endswith(".ukshader"))
    checked = 0
    drifted = []
    problems = []
    for name in names:
        path = os.path.join(shader_dir, name)
        doc = load_json(path)
        if not is_generated(doc):
            continue
        checked += 1
        fresh = compile_graph_json(doc["graph"], doc.get("groups"))
        if fresh["compiled"]["errors"]:
            problems.append((name, "; ".join(fresh["compiled"]["errors"])))
            continue
        for key, value in doc.items():
            if key not in fresh:
                fresh[key] = value
        data = sg_mirror.write_document(fresh)
        with open(path, "rb") as f:
            old = f.read()
        if data != old:
            drifted.append(name)
            if verbose:
                first = _first_diff(old.decode("utf-8"), data.decode("utf-8"))
                print(f"  {name}: {first}")
    for name, error in problems:
        print(f"  {name}: compile errors: {error}", file=sys.stderr)
    print(f"verified {checked} generated shaders: {len(drifted)} drifted, {len(problems)} failed")
    return not drifted and not problems


def _first_diff(a, b):
    la, lb = a.split("\n"), b.split("\n")
    for i in range(max(len(la), len(lb))):
        x = la[i] if i < len(la) else "<missing>"
        y = lb[i] if i < len(lb) else "<missing>"
        if x != y:
            return f"line {i + 1}\n    shipped: {x[:160]}\n    fresh:   {y[:160]}"
    return "identical"


def main(argv):
    if len(argv) < 3:
        print(__doc__.strip())
        return 2
    cmd = argv[1]
    if cmd == "compile" and len(argv) == 4:
        return 0 if compile_dir(argv[2], argv[3]) else 1
    if cmd == "refresh" and len(argv) == 3:
        count, problems = refresh(argv[2], write=True)
        for name, error in problems:
            print(f"  {name}: {error}", file=sys.stderr)
        print(f"refreshed {count} shaders")
        return 0 if not problems else 1
    if cmd == "verify" and len(argv) >= 3:
        return 0 if verify(argv[2], verbose="-v" in argv) else 1
    print(__doc__.strip())
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
