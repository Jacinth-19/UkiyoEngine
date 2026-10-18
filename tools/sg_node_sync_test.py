#!/usr/bin/env python3
"""Checks that the Python mirror of the shader graph matches the editor's C++ source.

  python3 tools/sg_node_sync_test.py

Compares, field by field:
  editor/src/shader_graph/sg_nodes.cpp    vs  tools/sg_mirror.py NODES
  editor/src/shader_graph/sg_codegen.cpp  vs  the mirror's boilerplate line lists

The mirror exists so the starter shaders can be built without Qt (see tools/sg_mirror.py). Nothing
stops the two from drifting, so this test parses the C++ and fails on any difference: a node added to
one table and not the other, a changed GLSL template, a reordered frame block.

It is a text comparison, not a build: run the real ukiyo_shader_tool roundtrip when Qt is available.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import sg_mirror  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
NODES_CPP = os.path.join(ROOT, "editor/src/shader_graph/sg_nodes.cpp")
CODEGEN_CPP = os.path.join(ROOT, "editor/src/shader_graph/sg_codegen.cpp")

CATEGORIES = {"kInput": "Input", "kMath": "Math", "kVector": "Vector", "kTexture": "Texture",
              "kProcedural": "Procedural", "kStylize": "Stylize", "kNpr": "NPR", "kMaster": "Master"}
KINDS = {"Kind::Float": "float", "Kind::Vec2": "vec2", "Kind::Vec3": "vec3"}
PARAMS = {"ParamKind::None": "none", "ParamKind::Float": "float", "ParamKind::Vec2": "vec2",
          "ParamKind::Color": "color", "ParamKind::Texture": "texture"}


def unescape(text):
    """C++ string literal body -> the string it denotes."""
    out = []
    i = 0
    while i < len(text):
        c = text[i]
        if c == "\\" and i + 1 < len(text):
            nxt = text[i + 1]
            out.append({"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\", "0": "\0"}.get(nxt, nxt))
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def string_run(source, start):
    """Reads one or more adjacent string literals from `start`; returns (joined text, next index).

    Handles the QStringLiteral("a" "b") concatenation the C++ uses for multi-line GLSL.
    """
    parts = []
    i = start
    while True:
        while i < len(source) and source[i] in " \t\r\n":
            i += 1
        if i >= len(source) or source[i] != '"':
            break
        j = i + 1
        body = []
        while j < len(source):
            if source[j] == "\\":
                body.append(source[j:j + 2])
                j += 2
                continue
            if source[j] == '"':
                break
            body.append(source[j])
            j += 1
        parts.append(unescape("".join(body)))
        i = j + 1
    return "".join(parts), i


def cpp_node_specs(source):
    """Parses every `{ NodeSpec n = node(...); ... add(n); }` block."""
    specs = {}
    order = []
    pattern = re.compile(r'\{\s*NodeSpec n = node\("([^"]+)",\s*(\w+),\s*"([^"]*)"\);(.*?)\badd\(n\);\s*\}', re.S)
    for match in pattern.finditer(source):
        node_id, category, caption, body = match.groups()
        spec = {
            "id": node_id,
            "category": CATEGORIES.get(category, category),
            "caption": caption,
            "inputs": parse_ports(body, "in"),
            "outputs": parse_ports(body, "out"),
            "param": parse_param(body),
            "out_expr": parse_out_expr(body),
            "decl": parse_single_string(body, "decl"),
            "helper": parse_single_string(body, "helper"),
            "master": re.search(r"n\.master\s*=\s*true\s*;", body) is not None,
            "surface_fields": parse_string_list(body, "surfaceFields"),
        }
        specs[node_id] = spec
        order.append(node_id)
    return specs, order


def parse_ports(body, field):
    m = re.search(r"n\." + field + r"\s*=\s*\{(.*?)\}\s*;", body, re.S)
    if not m:
        return []
    ports = []
    for pm in re.finditer(r'P\(\s*"([^"]*)"\s*,\s*(Kind::\w+)\s*(?:,\s*"((?:[^"\\]|\\.)*)")?\s*\)', m.group(1)):
        name, kind, default = pm.groups()
        ports.append({"name": name, "kind": KINDS[kind], "default": unescape(default) if default else ""})
    return ports


def parse_param(body):
    m = re.search(r"n\.param\s*=\s*(ParamKind::\w+)\s*;", body)
    return PARAMS[m.group(1)] if m else "none"


def parse_out_expr(body):
    m = re.search(r"n\.outExpr\s*=\s*outs\(\{(.*?)\}\)\s*;", body, re.S)
    if not m:
        return []
    return split_literals(m.group(1))


def parse_single_string(body, field):
    m = re.search(r"n\." + field + r"\s*=\s*QStringLiteral\(", body)
    if not m:
        return ""
    text, _ = string_run(body, m.end())
    return text


def parse_string_list(body, field):
    m = re.search(r"n\." + field + r"\s*=\s*\{(.*?)\}\s*;", body, re.S)
    if not m:
        return []
    return re.findall(r'QStringLiteral\("([^"]*)"\)', m.group(1))


def split_literals(text):
    """The list of string literals inside `{...}`, joining adjacent ones."""
    out = []
    i = 0
    while i < len(text):
        if text[i] == '"':
            value, i = string_run(text, i)
            out.append(value)
            continue
        i += 1
    return out


def cpp_literals(source):
    """Every QStringLiteral body in the file, in order."""
    out = []
    for m in re.finditer(r"QStringLiteral\(", source):
        value, _ = string_run(source, m.end())
        out.append(value)
    return out


def find_run(haystack, needle):
    """Index of `needle` as a contiguous run in `haystack`, or -1."""
    for i in range(len(haystack) - len(needle) + 1):
        if haystack[i:i + len(needle)] == needle:
            return i
    return -1


def main():
    problems = []
    with open(NODES_CPP, "r", encoding="utf-8") as f:
        nodes_src = f.read()
    with open(CODEGEN_CPP, "r", encoding="utf-8") as f:
        codegen_src = f.read()

    # A brace or paren left open by an edit would silently swallow the rest of the table.
    for name, src in (("sg_nodes.cpp", nodes_src), ("sg_codegen.cpp", codegen_src)):
        for open_ch, close_ch in (("{", "}"), ("(", ")")):
            if src.count(open_ch) != src.count(close_ch):
                problems.append(f"{name}: unbalanced {open_ch}{close_ch} "
                                f"({src.count(open_ch)} vs {src.count(close_ch)})")

    cpp_specs, cpp_order = cpp_node_specs(nodes_src)
    py_specs = {s.id: s for s in sg_mirror.NODES}

    for missing in sorted(set(cpp_specs) - set(py_specs)):
        problems.append(f"{missing}: in sg_nodes.cpp but not in tools/sg_mirror.py")
    for extra in sorted(set(py_specs) - set(cpp_specs)):
        problems.append(f"{extra}: in tools/sg_mirror.py but not in sg_nodes.cpp")

    for node_id in cpp_order:
        if node_id not in py_specs:
            continue
        c = cpp_specs[node_id]
        p = py_specs[node_id]
        checks = [
            ("category", c["category"], p.category),
            ("caption", c["caption"], p.caption),
            ("param", c["param"], p.param),
            ("master", c["master"], p.master),
            ("out_expr", c["out_expr"], p.out_expr),
            ("decl", c["decl"], p.decl),
            ("helper", c["helper"], p.helper),
            ("surface_fields", c["surface_fields"], p.surface_fields),
            ("inputs", [(q["name"], q["kind"], q["default"]) for q in c["inputs"]],
             [(q.name, q.kind, q.default) for q in p.inputs]),
            ("outputs", [(q["name"], q["kind"], q["default"]) for q in c["outputs"]],
             [(q.name, q.kind, q.default) for q in p.outputs]),
        ]
        for field, cpp_value, py_value in checks:
            if cpp_value != py_value:
                problems.append(f"{node_id}.{field}:\n    cpp: {cpp_value!r}\n    py:  {py_value!r}")

    # The generated boilerplate: each Python line list must appear verbatim, in order, in the C++.
    literals = cpp_literals(codegen_src)
    for name, lines in (("FRAME_BLOCK_LINES", sg_mirror.FRAME_BLOCK_LINES),
                        ("VARYING_LINES", sg_mirror.VARYING_LINES),
                        ("SURFACE_STRUCT_LINES", sg_mirror.SURFACE_STRUCT_LINES),
                        ("SURFACE_DEFAULTS", sg_mirror.SURFACE_DEFAULTS),
                        ("MAIN_LINES", sg_mirror.MAIN_LINES)):
        if not lines:
            continue
        if find_run(literals, list(lines)) < 0:
            at = find_run(literals, [lines[0]])
            if at < 0:
                problems.append(f"{name}: its first line is not in sg_codegen.cpp")
                continue
            for k, line in enumerate(lines):
                if at + k >= len(literals) or literals[at + k] != line:
                    want = literals[at + k] if at + k < len(literals) else "<end of file>"
                    problems.append(f"{name}[{k}]:\n    cpp: {want!r}\n    py:  {line!r}")
                    break

    masters = [s.id for s in sg_mirror.NODES if s.master]
    print(f"{len(cpp_specs)} C++ node specs, {len(py_specs)} mirrored, {len(masters)} masters: "
          f"{', '.join(masters)}")
    if problems:
        print(f"\n{len(problems)} differences:")
        for p in problems:
            print("  " + p)
        return 1
    print("node library and generated boilerplate are in sync")
    return 0


if __name__ == "__main__":
    sys.exit(main())
