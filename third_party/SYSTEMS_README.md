# Vendored systems libraries

Copied from upstream without their git history. Only the parts the engine builds are kept.

| Directory | Upstream | Commit | Licence | Used for |
|---|---|---|---|---|
| `JoltPhysics/` | https://github.com/jrouwe/JoltPhysics | f187a24 | MIT (`LICENSE`) | Rigid bodies, collision, ray casts |
| `recastnavigation/` | https://github.com/recastnavigation/recastnavigation | 9f4ce64 | zlib (`License.txt`) | Recast (navmesh baking), Detour, DetourCrowd |
| `lua/` | https://github.com/lua/lua | v5.4.9 | MIT (`LICENSE.txt`) | Lua 5.4.9 core and standard libraries (no standalone interpreter) |
| `miniaudio/` | https://github.com/mackron/miniaudio | 9634bed (v0.11.25) | MIT or public domain | Audio output, mixing, 3D sound |

Build notes:
- Jolt is built with its own CMake (`JoltPhysics/Build`). Its Vulkan, DirectX, and Metal backends are off
  (`JPH_USE_VK=OFF` and the others), because the engine does not use them.
- Recast, Detour, and DetourCrowd are built from source by the root `CMakeLists.txt`. The upstream demo, test, and debug-util directories are not vendored.
- Lua is built without `lua.c`, `luac.c`, `ltests.c`, and `onelua.c`.
