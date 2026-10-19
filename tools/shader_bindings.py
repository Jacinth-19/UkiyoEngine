#!/usr/bin/env python3
"""Audit descriptor bindings of every shader, per backend, without a GPU.

Two kinds of mistake are invisible to a GLSL compiler and to a build with no device:

  * two samplers declared at the same (set, binding). glslang accepts it for GL, and on Vulkan the two variables
    alias one descriptor, so one of them reads the wrong image. This is exactly how gi_trace.frag once read its
    depth buffer out of the previous frame's colour.
  * two bindings in one shader that land on the same texture unit in the GL backend, which flattens every set into
    one unit space (`glBindTextureUnit(binding, ...)`). A pass that binds both the Frame set (shadow atlas on 1)
    and the Post set (colour on 1) has one of the two silently overwritten.

The audit resolves `#include` the same way the shader compiler does, then checks, for every shader:
  Vulkan: no repeated (set, binding) for samplers, and none for uniform buffers.
  GL:     no repeated texture unit (the binding number) and no repeated buffer unit.

Exit status is 1 with a list when anything collides.
"""
import re
import sys
from pathlib import Path

DECL = re.compile(r"^\s*(TEX|UBO)\(\s*(\d+)\s*,\s*(\d+)\s*\)\s*uniform\s+\w+\s+(\w+)")
INCLUDE = re.compile(r'^\s*#include\s+"([^"]+)"')


def expand(path, seen=None):
    """The shader's lines with includes inlined, in declaration order."""
    seen = seen or set()
    if path in seen:
        return []
    seen.add(path)
    out = []
    for line in path.read_text().splitlines():
        m = INCLUDE.match(line)
        if m:
            inc = path.parent / m.group(1)
            if inc.exists():
                out.extend(expand(inc, seen))
        else:
            out.append(line)
    return out


def declarations(path):
    """[(kind, set, binding, name, line)] for one shader after include expansion."""
    found = []
    for line in expand(path):
        m = DECL.match(line)
        if m:
            found.append((m.group(1), int(m.group(2)), int(m.group(3)), m.group(4), line.strip()))
    return found


def audit(shaders):
    problems = []
    for path in sorted(shaders):
        decls = declarations(path)
        # Vulkan: (set, binding) identifies a descriptor.
        for kind in ("TEX", "UBO"):
            seen = {}
            for d in decls:
                if d[0] != kind:
                    continue
                key = (d[1], d[2])
                if key in seen:
                    problems.append(f"{path.name}: {kind} set {key[0]} binding {key[1]} declared by both "
                                    f"'{seen[key]}' and '{d[3]}' (Vulkan aliases them)")
                else:
                    seen[key] = d[3]
        # GL: sets share one unit space, so the binding alone must be unique per kind.
        for kind in ("TEX", "UBO"):
            seen = {}
            for d in decls:
                if d[0] != kind:
                    continue
                if d[2] in seen:
                    name, other_set = seen[d[2]]
                    problems.append(f"{path.name}: {kind} binding {d[2]} used by '{name}' (set {other_set}) and "
                                    f"'{d[3]}' (set {d[1]}) — collides on the shared GL unit space")
                else:
                    seen[d[2]] = (d[3], d[1])
    return problems


def main(argv):
    root = Path(argv[1] if len(argv) > 1 else "shaders")
    shaders = sorted(list(root.glob("*.vert")) + list(root.glob("*.frag")))
    if not shaders:
        print(f"no shaders in {root}", file=sys.stderr)
        return 2
    problems = audit(shaders)
    for p in problems:
        print(p)
    print(f"[bindings] {len(shaders)} shaders, {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
