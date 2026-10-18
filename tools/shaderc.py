#!/usr/bin/env python3
"""Ukiyo shader build step.

Every shader in shaders/ is written once in GLSL 450. This script:
  * expands `#include "file"` directives (no GL_GOOGLE_include_directive needed),
  * prepends `#version 450 core` plus a backend define,
  * writes OpenGL sources to  <out>/gl/<name>.glsl       (loaded at runtime)
  * compiles Vulkan SPIR-V    to <out>/spv/<name>.spv    (via glslangValidator)

Usage: shaderc.py <shader_dir> <out_dir> [glslangValidator]
"""
import os
import re
import subprocess
import sys
import tempfile

INCLUDE_RE = re.compile(r'^\s*#include\s+"([^"]+)"\s*$')


def expand(path, depth=0):
    if depth > 16:
        raise RuntimeError(f"include depth exceeded in {path}")
    out = []
    base = os.path.dirname(path)
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            m = INCLUDE_RE.match(line)
            if m:
                inc = os.path.join(base, m.group(1))
                out.append(f"// ---- begin include {m.group(1)} ----\n")
                out.append(expand(inc, depth + 1))
                out.append(f"// ---- end include {m.group(1)} ----\n")
            else:
                out.append(line)
    return "".join(out)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    shader_dir, out_dir = sys.argv[1], sys.argv[2]
    glslang = sys.argv[3] if len(sys.argv) > 3 else "glslangValidator"
    gl_dir = os.path.join(out_dir, "gl")
    spv_dir = os.path.join(out_dir, "spv")
    os.makedirs(gl_dir, exist_ok=True)
    os.makedirs(spv_dir, exist_ok=True)

    failures = 0
    for name in sorted(os.listdir(shader_dir)):
        stem, ext = os.path.splitext(name)
        if ext not in (".vert", ".frag"):
            continue
        src = os.path.join(shader_dir, name)
        body = expand(src)

        # OpenGL: plain GLSL 450 text, read at runtime.
        gl_text = "#version 450 core\n#define BACKEND_GL 1\n" + body
        with open(os.path.join(gl_dir, name + ".glsl"), "w", encoding="utf-8") as f:
            f.write(gl_text)

        # Vulkan: compile to SPIR-V at build time.
        vk_text = "#version 450 core\n#define BACKEND_VK 1\n" + body
        stage = "vert" if ext == ".vert" else "frag"
        with tempfile.NamedTemporaryFile("w", suffix=f"{ext}", delete=False) as tmp:
            tmp.write(vk_text)
            tmp_path = tmp.name
        out_spv = os.path.join(spv_dir, name + ".spv")
        res = subprocess.run(
            [glslang, "-V", "--target-env", "vulkan1.3", "-S", stage, "-o", out_spv, tmp_path],
            capture_output=True, text=True)
        os.unlink(tmp_path)
        if res.returncode != 0:
            failures += 1
            sys.stderr.write(f"[shaderc] {name} failed:\n{res.stdout}{res.stderr}\n")
        else:
            print(f"[shaderc] {name} ok")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
