# Vendored dependencies

The engine builds from a checkout with nothing fetched at configure time. Everything the build needs that is not part
of the operating system lives here.

| Directory | Version | Licence | Why it is vendored |
| --- | --- | --- | --- |
| `glm/` | 0.9.9.8 | MIT (`glm/copying.txt`) | Header-only; every translation unit includes it |
| `nlohmann/` | 3.11.3 | MIT (`nlohmann/LICENSE.MIT`) | Single header; scene files, input assets, project files |
| `glfw/` | 3.4 | zlib (`glfw/LICENSE.md`) | Windowing and input; built as a subdirectory |
| `JoltPhysics/` | (checked in) | MIT | Rigid bodies |
| `recastnavigation/` | (checked in) | zlib | Navigation |
| `lua/` | 5.4.9 | MIT | Scripting |
| `astcenc/` | 5.7.0 | Apache-2.0 (`astcenc/LICENSE.txt`) | The ASTC texture encoder (batch 7's phone format): an ASTC encoder is a search, not a formula. Core library sources only — the CLI, unit tests, fuzzers and GoogleTest are not copied. Built scalar (`none_4` vector path) so one source set serves arm64 and x86_64 ABIs invariantly. Upstream: Arm-software/astc-encoder, pinned at tag 5.7.0 |
| `meshoptimizer/`, `cgltf.h`, `stb_*.h`, `miniaudio`, `nodeeditor`, `glad`, `vk_mem_alloc.h` | (checked in) | see each header | Single-file or single-directory libraries |

## What is deliberately *not* vendored

Three dependencies stay on the system, and the reason is size or the loader rather than preference:

* **Qt 6** — the editor and the launcher are Qt applications. Vendoring Qt means shipping an entire platform SDK
  (several hundred megabytes, with its own licence file per module) to save one `apt-get install`. It is a documented
  package requirement in the README instead.
* **glslang** — `glslang-dev` is linked into the engine (generated material shaders are compiled to SPIR-V at
  runtime) and `glslang-tools` supplies `glslangValidator` to `tools/shaderc.py` at build time. The source tree is
  roughly forty thousand files; the distro package is smaller than the vendored copy's build time and carries the
  ABI the SPIR-V tools are tested against.
* **The Vulkan loader** — `find_package(Vulkan)` provides the loader the engine links and the ICD JSONs that make it
  find a driver. A driver is never vendorable: it belongs to the machine the engine runs on.

If any of the three ever needs pinning, the place to start is a container image or a release artifact, not a
multi-hundred-megabyte directory in the repository.

## Upgrading one

The directories are plain copies, with documentation, tests and examples removed to keep the checkout small. To
upgrade, take the same subset from the tag and update the version in this table:

```sh
git clone --depth 1 --branch <tag> https://github.com/g-truc/glm.git /tmp/glm
rm -rf third_party/glm/glm && cp -r /tmp/glm/glm third_party/glm/glm && cp /tmp/glm/copying.txt third_party/glm/
```

```sh
git clone --depth 1 --branch v3.11.3 https://github.com/nlohmann/json.git /tmp/json
rm -rf third_party/nlohmann/include/nlohmann
cp -r /tmp/json/single_include/nlohmann third_party/nlohmann/include/nlohmann
```

```sh
git clone --depth 1 --branch 3.4 https://github.com/glfw/glfw.git /tmp/glfw
for d in CMake CMakeLists.txt LICENSE.md README.md deps include src; do
  rm -rf "third_party/glfw/$d" && cp -r "/tmp/glfw/$d" "third_party/glfw/$d"
done
```

`docs/`, `examples/` and `tests/` are intentionally absent from the GLFW copy; its `CMakeLists.txt` builds them only
when `GLFW_STANDALONE` is set, which it is not inside this tree.

## What CI gained

Before vendoring, the workflow installed `libglfw3-dev libglm-dev nlohmann-json3-dev` on every run and the whole
install step took about thirty seconds. Those three packages are gone; the remaining install is the toolchain,
the Vulkan loader, the Qt SDKs and glslang. The measured before/after is in [docs/INPUT.md](../docs/INPUT.md#ci).
