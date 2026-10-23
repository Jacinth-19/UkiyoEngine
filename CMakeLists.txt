cmake_minimum_required(VERSION 3.20)
project(UkiyoEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- Compiler flags --------------------------------------------------------
# One variable per intent, so no target has to know which compiler is building it. CI builds both: Ubuntu with GCC and
# Windows with MSVC. The MSVC set keeps the same level of noise: /W3 is its -Wall, /permissive- rejects the GNU
# extensions GCC accepts by default, and /utf-8 matters because the sources contain Japanese and box drawing.
if(MSVC)
  set(UKIYO_WARNINGS /W3 /permissive- /utf-8)
  set(UKIYO_WARNINGS_PEDANTIC ${UKIYO_WARNINGS})
  set(UKIYO_NO_WARNINGS /w)
  # min() and max() from <windows.h> collide with std::min/std::max; the CRT's "unsafe function" warnings are noise
  # for a codebase that already uses the bounded forms.
  # M_PI is not defined by <cmath> under MSVC unless asked for; the audio tests and the splash read it.
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
else()
  set(UKIYO_WARNINGS -Wall -Wextra -Wno-missing-field-initializers)
  set(UKIYO_WARNINGS_PEDANTIC -Wall -Wextra -Wpedantic)
  set(UKIYO_NO_WARNINGS -w)
endif()

# What the engine is made of, in one place: the desktop targets and the Android app build the same lists.
include(cmake/ukiyo_sources.cmake)

# ---- Android -----------------------------------------------------------------------------------
# The editor is a desktop Qt program and there is no Android application yet, so an Android configure does not build a
# window, a renderer or a hub. What it does build is the portable core: the rules the editor's Project browser runs on
# (the glob/fuzzy query grammar and the thumbnail ladder), which include no Qt and no graphics header. Compiling those
# for the two ABIs the project ships is what makes "Android" a build that can be checked rather than a promise, and it
# keeps the phone build honest: the same translation units the desktop runs are the ones the NDK compiles.
if(ANDROID)
  # The player's runtime core is portable by construction (no Qt, no GL, no Vulkan): the manifest, the scene reader, the
  # transform and light maths, the HUD canvas and its font atlas, and the HUD layout. Compiling it for both ABIs is what
  # keeps "the player runs on Android" a checked claim rather than an intention, in the same way the browser's rules are.
  set(UKIYO_ANDROID_CORE
    src/engine/search.cpp
    src/engine/browser_zoom.cpp
    src/engine/transform.cpp
    src/engine/lights.cpp
    src/engine/build_manifest.cpp
    src/engine/scene_runtime.cpp
    src/engine/async_load.cpp
    src/engine/text_atlas.cpp
    src/engine/msdf.cpp
    src/engine/hud.cpp
    src/player/hud_layout.cpp)
  # game_data_test is the first game's project read through the same portable readers a phone would use: it is built
  # for both ABIs here, and run on the desktop jobs.
  foreach(t search_test browser_zoom_test transform_test lights_test manifest_test scene_runtime_test game_data_test)
    add_executable(${t} tests/${t}.cpp ${UKIYO_ANDROID_CORE})
    target_include_directories(${t} PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party
                               ${CMAKE_SOURCE_DIR}/third_party/glm ${CMAKE_SOURCE_DIR}/third_party/nlohmann/include)
    target_compile_options(${t} PRIVATE ${UKIYO_WARNINGS})
  endforeach()

  # ---- The first game, on a phone --------------------------------------------------------------
  # The same engine the desktop builds, minus the three translation units that cannot exist here, plus the NDK's
  # native-activity glue. Vulkan is the only backend (Android has no desktop GL, so the GL backend and GLAD are out),
  # and the runtime GLSL compiler goes with glslang, which the NDK does not ship — shader_compiler_stub.cpp answers for
  # it, because a shipped game carries its compiled shaders and never compiles a graph on the device.
  #
  # The third-party libraries are configured here rather than after the early return, because the app links the whole
  # engine: physics, navigation, scripting, audio and the renderer.
  add_library(ukiyo_glm INTERFACE)
  target_include_directories(ukiyo_glm INTERFACE ${CMAKE_SOURCE_DIR}/third_party/glm)
  add_library(glm::glm ALIAS ukiyo_glm)
  add_library(ukiyo_json INTERFACE)
  target_include_directories(ukiyo_json INTERFACE ${CMAKE_SOURCE_DIR}/third_party/nlohmann/include)
  add_library(nlohmann_json::nlohmann_json ALIAS ukiyo_json)

  file(GLOB LUA_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/third_party/lua/*.c)
  add_library(ukiyo_lua STATIC ${LUA_SRC})
  target_include_directories(ukiyo_lua PUBLIC ${CMAKE_SOURCE_DIR}/third_party/lua)
  target_compile_definitions(ukiyo_lua PRIVATE LUA_USE_LINUX)
  target_compile_options(ukiyo_lua PRIVATE ${UKIYO_NO_WARNINGS})

  add_library(ukiyo_miniaudio STATIC ${CMAKE_SOURCE_DIR}/third_party/miniaudio/miniaudio_impl.cpp)
  target_include_directories(ukiyo_miniaudio PUBLIC ${CMAKE_SOURCE_DIR}/third_party/miniaudio)
  target_compile_options(ukiyo_miniaudio PRIVATE ${UKIYO_NO_WARNINGS})
  target_link_libraries(ukiyo_miniaudio PUBLIC ${CMAKE_DL_LIBS} m)

  file(GLOB RECAST_SRC CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/third_party/recastnavigation/Recast/Source/*.cpp
    ${CMAKE_SOURCE_DIR}/third_party/recastnavigation/Detour/Source/*.cpp
    ${CMAKE_SOURCE_DIR}/third_party/recastnavigation/DetourCrowd/Source/*.cpp)
  add_library(ukiyo_recast STATIC ${RECAST_SRC})
  target_include_directories(ukiyo_recast PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/recastnavigation/Recast/Include
    ${CMAKE_SOURCE_DIR}/third_party/recastnavigation/Detour/Include
    ${CMAKE_SOURCE_DIR}/third_party/recastnavigation/DetourCrowd/Include)
  target_compile_options(ukiyo_recast PRIVATE ${UKIYO_NO_WARNINGS})

  # Jolt: the CPU build with the same options the desktop uses, and without the Vulkan compute shaders it would need
  # dxc for.
  set(JPH_BUILD_SHARED_LIBS OFF CACHE BOOL "Jolt: shared library" FORCE)
  set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "Jolt: link-time optimisation" FORCE)
  set(GENERATE_DEBUG_SYMBOLS OFF CACHE BOOL "Jolt: debug symbols" FORCE)
  set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "Jolt: all warnings" FORCE)
  set(USE_ASSERTS OFF CACHE BOOL "Jolt: asserts in Release" FORCE)
  set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "Jolt: profiler" FORCE)
  set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "Jolt: debug renderer" FORCE)
  set(JPH_USE_VK OFF CACHE BOOL "Jolt: Vulkan compute" FORCE)
  add_subdirectory(third_party/JoltPhysics/Build ${CMAKE_BINARY_DIR}/jolt EXCLUDE_FROM_ALL)

  set(UKIYO_CORE_ANDROID ${UKIYO_CORE_SOURCES})
  list(REMOVE_ITEM UKIYO_CORE_ANDROID
    src/rhi/gl_device.cpp
    src/engine/shader_compiler.cpp
    third_party/glad/src/gl.c)
  list(APPEND UKIYO_CORE_ANDROID src/engine/shader_compiler_stub.cpp)

  # The NDK's sysroot carries libvulkan.so; a toolchain that cannot find it fails loudly here rather than at link time.
  find_library(UKIYO_VULKAN_LIBRARY vulkan)
  if(NOT UKIYO_VULKAN_LIBRARY)
    message(FATAL_ERROR "libvulkan.so was not found in the NDK sysroot")
  endif()
  add_library(ukiyo_core_android STATIC ${UKIYO_CORE_ANDROID})
  target_include_directories(ukiyo_core_android PUBLIC
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party ${CMAKE_SOURCE_DIR}/third_party/meshoptimizer)
  # VK_USE_PLATFORM_ANDROID_KHR is what makes <vulkan/vulkan.h> declare vkCreateAndroidSurfaceKHR.
  target_compile_definitions(ukiyo_core_android PUBLIC
    UKIYO_SHADER_DIR="shaders/spv" VK_USE_PLATFORM_ANDROID_KHR=1)
  target_link_libraries(ukiyo_core_android PUBLIC
    ${UKIYO_VULKAN_LIBRARY} android log glm::glm nlohmann_json::nlohmann_json ${CMAKE_DL_LIBS})
  target_compile_options(ukiyo_core_android PRIVATE ${UKIYO_WARNINGS})

  add_library(ukiyo_input STATIC ${UKIYO_INPUT_SOURCES})
  target_include_directories(ukiyo_input PUBLIC ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(ukiyo_input PUBLIC glm::glm nlohmann_json::nlohmann_json)
  target_compile_options(ukiyo_input PRIVATE ${UKIYO_WARNINGS})

  add_library(ukiyo_systems_android STATIC ${UKIYO_SYSTEMS_SOURCES})
  target_include_directories(ukiyo_systems_android PUBLIC ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(ukiyo_systems_android PUBLIC
    ukiyo_core_android ukiyo_input Jolt ukiyo_recast ukiyo_lua ukiyo_miniaudio)
  target_compile_options(ukiyo_systems_android PRIVATE ${UKIYO_WARNINGS})

  # The app: android_main.cpp is the desktop player's presentation layer with the platform swapped, and the glue turns
  # it into the entry point an Android activity starts.
  add_library(ukiyo_player_android SHARED
    src/player/android_main.cpp
    src/player/player_app.cpp
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
  set_target_properties(ukiyo_player_android PROPERTIES OUTPUT_NAME "ukiyo_player")
  target_include_directories(ukiyo_player_android PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${ANDROID_NDK}/sources/android/native_app_glue)
  target_link_libraries(ukiyo_player_android PRIVATE ukiyo_systems_android ukiyo_core_android android log)
  # The glue's entry point is referenced by nothing: keep it alive through the link.
  target_link_options(ukiyo_player_android PRIVATE "-u ANativeActivity_onCreate")
  target_compile_options(ukiyo_player_android PRIVATE ${UKIYO_WARNINGS})
  return()
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_package(Vulkan REQUIRED)

# ---- Vendored dependencies -------------------------------------------------
# GLM, nlohmann_json and GLFW live in third_party/ so a checkout builds without fetching anything and CI does not
# spend a package install on them (see third_party/VENDOR.md for versions and the upgrade commands). Each one falls
# back to the system package if the directory is missing, so an older checkout still configures.
if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/glm/glm/glm.hpp)
  add_library(ukiyo_glm INTERFACE)
  # No compile definitions: the vendored copy must behave exactly like the system package it replaces.
  target_include_directories(ukiyo_glm INTERFACE ${CMAKE_SOURCE_DIR}/third_party/glm)
  add_library(glm::glm ALIAS ukiyo_glm)
  message(STATUS "glm: vendored (third_party/glm)")
else()
  find_package(glm REQUIRED)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/nlohmann/include/nlohmann/json.hpp)
  add_library(ukiyo_json INTERFACE)
  target_include_directories(ukiyo_json INTERFACE ${CMAKE_SOURCE_DIR}/third_party/nlohmann/include)
  add_library(nlohmann_json::nlohmann_json ALIAS ukiyo_json)
  message(STATUS "nlohmann_json: vendored (third_party/nlohmann)")
else()
  find_package(nlohmann_json REQUIRED)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/glfw/CMakeLists.txt)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "Build GLFW's examples")
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "Build GLFW's tests")
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "Build GLFW's documentation")
  set(GLFW_INSTALL OFF CACHE BOOL "Add GLFW's install rules")
  # GLFW 3.4 builds the Wayland backend by default, which needs wayland-scanner and the wayland protocols at
  # configure time. The editor is an X11 application here and CI has no Wayland toolchain, so X11 is the default.
  # Pass -DGLFW_BUILD_WAYLAND=ON to build it where the headers and scanner are installed.
  set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "Build GLFW's Wayland backend")
  add_subdirectory(third_party/glfw EXCLUDE_FROM_ALL)
  message(STATUS "glfw: vendored (third_party/glfw)")
else()
  find_package(glfw3 REQUIRED)
endif()
# The Vulkan SDK ships glslangValidator and the glslang package, but neither is on CMake's default search path: the
# SDK puts tools in Bin/ and the package config in Lib/cmake (capital L on Windows, lowercase on Linux). Debian splits
# glslang out into libglslang-dev, where the default path already finds it, hence QUIET first and REQUIRED after.
find_program(GLSLANG_VALIDATOR glslangValidator
  HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
if(NOT GLSLANG_VALIDATOR)
  message(FATAL_ERROR "glslangValidator not found: install the Vulkan SDK, or glslang-tools on Debian")
endif()
# glslang is linked into the engine: GLSL -> SPIR-V at runtime (generated material shaders).
#
# On Linux it is libglslang-dev and the default search finds it. On Windows the Vulkan SDK does ship a package, but its
# SPIRV-Tools imported target is generated with the wrong install prefix — it points at
# <sdk>/Lib/cmake/lib/SPIRV-Tools.lib, a file that does not exist — so including it fails inside the SDK's own file.
# CI therefore installs glslang with vcpkg and puts that prefix on CMAKE_PREFIX_PATH (see .github/workflows/build.yml);
# a developer on Windows does the same, or installs glslang themselves.
find_package(glslang CONFIG QUIET HINTS "$ENV{VULKAN_SDK}/Lib/cmake" "$ENV{VULKAN_SDK}/lib/cmake")
if(NOT glslang_FOUND)
  find_package(glslang CONFIG REQUIRED)
endif()

# ---- Shaders: tools/shaderc.py -> build/shaders/{gl,spv} ------------------
set(SHADER_SRC_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_OUT_DIR ${CMAKE_BINARY_DIR}/shaders)
file(GLOB SHADER_SOURCES CONFIGURE_DEPENDS
  ${SHADER_SRC_DIR}/*.vert ${SHADER_SRC_DIR}/*.frag ${SHADER_SRC_DIR}/*.glsl)
set(SHADER_STAMP ${SHADER_OUT_DIR}/.stamp)
add_custom_command(
  OUTPUT ${SHADER_STAMP}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUT_DIR}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/shaderc.py
          ${SHADER_SRC_DIR} ${SHADER_OUT_DIR} ${GLSLANG_VALIDATOR}
  COMMAND ${CMAKE_COMMAND} -E touch ${SHADER_STAMP}
  DEPENDS ${SHADER_SOURCES} ${CMAKE_SOURCE_DIR}/tools/shaderc.py
  COMMENT "Compiling shaders (GL + SPIR-V)")
add_custom_target(ukiyo_shaders ALL DEPENDS ${SHADER_STAMP})

# ---- Third-party systems (vendored under third_party/) ---------------------
# Jolt Physics: rigid bodies and collision. Only the library target is built.
# These are defaults, not overrides: a value given on the command line (-DUSE_ASSERTS=ON) or already in the
# cache wins. (They used to be FORCEd, which silently replaced any -D value on every configure.)
set(JPH_BUILD_SHARED_LIBS OFF CACHE BOOL "Build Jolt as a shared library")
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "Jolt: link-time optimisation")
set(GENERATE_DEBUG_SYMBOLS OFF CACHE BOOL "Jolt: debug symbols")
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "Jolt: all warnings")
set(USE_ASSERTS OFF CACHE BOOL "Jolt: keep asserts in Release")
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "Jolt: profiler in Debug and Release")
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "Jolt: debug renderer in Debug and Release")
# Jolt defaults to the static MSVC runtime (/MT). This build uses the dynamic one (/MD), and Qt only comes that way, so
# a /MT Jolt cannot be linked into the editor at all.
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "Jolt: static MSVC runtime")
# Jolt compiles its Vulkan compute shaders with dxc, looked up beside glslc. Without both, build Jolt
# CPU-only. The engine uses Jolt on the CPU, so nothing is lost. An explicit -DJPH_USE_VK=ON still wins;
# it is then a warning, because the Vulkan build needs dxc.
set(JPH_VK_DXC "")
if(Vulkan_GLSLC_EXECUTABLE)
  string(REPLACE "glslc" "dxc" JPH_VK_DXC "${Vulkan_GLSLC_EXECUTABLE}")
endif()
if(NOT JPH_VK_DXC OR NOT EXISTS "${JPH_VK_DXC}")
  message(STATUS "glslc or dxc not found: Jolt builds without Vulkan compute shaders")
  set(JPH_USE_VK OFF CACHE BOOL "Use Vulkan")
  if(JPH_USE_VK)
    message(WARNING "JPH_USE_VK=ON but glslc/dxc were not found: the Jolt Vulkan build will fail")
  endif()
endif()
add_subdirectory(third_party/JoltPhysics/Build EXCLUDE_FROM_ALL)

# Recast and Detour (navigation mesh, path queries, and DetourCrowd agents).
file(GLOB RECAST_SRC CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/third_party/recastnavigation/Recast/Source/*.cpp
  ${CMAKE_SOURCE_DIR}/third_party/recastnavigation/Detour/Source/*.cpp
  ${CMAKE_SOURCE_DIR}/third_party/recastnavigation/DetourCrowd/Source/*.cpp)
add_library(ukiyo_recast STATIC ${RECAST_SRC})
target_include_directories(ukiyo_recast PUBLIC
  ${CMAKE_SOURCE_DIR}/third_party/recastnavigation/Recast/Include
  ${CMAKE_SOURCE_DIR}/third_party/recastnavigation/Detour/Include
  ${CMAKE_SOURCE_DIR}/third_party/recastnavigation/DetourCrowd/Include)
target_compile_options(ukiyo_recast PRIVATE ${UKIYO_NO_WARNINGS})

# Lua 5.4 (scripting). Core and standard libraries only; the standalone interpreter is not built.
file(GLOB LUA_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/third_party/lua/*.c)
add_library(ukiyo_lua STATIC ${LUA_SRC})
target_include_directories(ukiyo_lua PUBLIC ${CMAKE_SOURCE_DIR}/third_party/lua)
if(WIN32)
  target_compile_definitions(ukiyo_lua PRIVATE LUA_USE_WINDOWS)
else()
  target_compile_definitions(ukiyo_lua PRIVATE LUA_USE_LINUX)
endif()
target_compile_options(ukiyo_lua PRIVATE ${UKIYO_NO_WARNINGS})

# miniaudio (audio output, mixing, and 3D sound).
add_library(ukiyo_miniaudio STATIC ${CMAKE_SOURCE_DIR}/third_party/miniaudio/miniaudio_impl.cpp)
target_include_directories(ukiyo_miniaudio PUBLIC ${CMAKE_SOURCE_DIR}/third_party/miniaudio)
target_compile_options(ukiyo_miniaudio PRIVATE ${UKIYO_NO_WARNINGS})
if(UNIX)
  # miniaudio's ALSA/PulseAudio backends open their libraries with dlopen and mix on its own thread; Windows uses
  # WASAPI and the Win32 thread API, which need no extra library.
  target_link_libraries(ukiyo_miniaudio PUBLIC ${CMAKE_DL_LIBS} pthread m)
endif()

# ---- Engine core library ---------------------------------------------------
add_library(ukiyo_core STATIC ${UKIYO_CORE_SOURCES})
add_dependencies(ukiyo_core ukiyo_shaders)
target_include_directories(ukiyo_core PUBLIC
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party
  ${CMAKE_SOURCE_DIR}/third_party/meshoptimizer
  ${CMAKE_SOURCE_DIR}/third_party/glad/include)
target_compile_definitions(ukiyo_core PUBLIC UKIYO_SHADER_DIR="${SHADER_OUT_DIR}")
target_link_libraries(ukiyo_core PUBLIC
  Vulkan::Vulkan glfw glm::glm nlohmann_json::nlohmann_json glslang::glslang glslang::SPIRV
  glslang::glslang-default-resource-limits ${CMAKE_DL_LIBS})
if(UNIX AND NOT APPLE)
  target_link_libraries(ukiyo_core PUBLIC pthread)
endif()
target_compile_options(ukiyo_core PRIVATE ${UKIYO_WARNINGS})

# ---- Game systems: physics, navigation, audio, animation state, scripting, particles -----
add_library(ukiyo_systems STATIC ${UKIYO_SYSTEMS_SOURCES})
target_include_directories(ukiyo_systems PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ukiyo_systems PUBLIC ukiyo_core ukiyo_input Jolt ukiyo_recast ukiyo_lua ukiyo_miniaudio)
if(WIN32)
  target_link_libraries(ukiyo_systems PUBLIC ws2_32)
endif()
target_compile_options(ukiyo_systems PRIVATE ${UKIYO_WARNINGS})

add_executable(systems_test tests/systems_test.cpp)
# The Lua scripting system: the sandbox, require(), error reporting, the per-entity runtime, and the files a
# project needs for an external editor. No device, no window.
add_executable(script_test tests/script_test.cpp)
target_link_libraries(script_test PRIVATE ukiyo_systems)
target_compile_options(script_test PRIVATE ${UKIYO_WARNINGS})
if(MSVC)
  # A crash on Windows arrives as an exit code with no words. The test's handler writes module+offset for the faulting
  # instruction and for the frames above it, and this map file is what turns those offsets back into function names.
  target_link_options(script_test PRIVATE "/MAP:${CMAKE_BINARY_DIR}/script_test.map")
endif()
add_executable(pmx_glb_test tests/pmx_glb_test.cpp src/engine/pmx_glb.cpp)
target_include_directories(pmx_glb_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(pmx_glb_test PRIVATE ${UKIYO_WARNINGS_PEDANTIC})
add_executable(ukiyo_pak tools/ukiyo_pak.cpp)
target_link_libraries(ukiyo_pak PRIVATE ukiyo_core)

# The packer (docs/PACKAGING.md): a project plus a player binary into a folder someone can run, and a zip of it. The
# library is separate so tests/pack_test.cpp drives it without a command line in the way.
add_library(ukiyo_pack_core STATIC src/pack/pack.cpp src/pack/pack_cli.cpp)
target_link_libraries(ukiyo_pack_core PUBLIC ukiyo_core)
target_compile_options(ukiyo_pack_core PRIVATE ${UKIYO_WARNINGS})
add_executable(ukiyo_pack tools/ukiyo_pack.cpp)
target_link_libraries(ukiyo_pack PRIVATE ukiyo_pack_core)
target_compile_options(ukiyo_pack PRIVATE ${UKIYO_WARNINGS})

# The first game's geometry (docs/GAME.md), generated so the committed GLBs can be rebuilt and checked.
add_executable(lantern_assets tools/lantern_assets.cpp)
target_link_libraries(lantern_assets PRIVATE glm::glm nlohmann_json::nlohmann_json)
target_compile_options(lantern_assets PRIVATE ${UKIYO_WARNINGS})

# Offline terrain preview (docs/TERRAIN.md): renders the Terrain Lab tile without a GPU. It links only the pure CPU
# terrain, layer and grass code, so the build needs no Vulkan, GL or GLFW for it, and CI compiles it on every push.
# STB is implemented in the tool itself, which is why it does not link ukiyo_core.
add_executable(terrain_preview tools/terrain_preview.cpp src/engine/terrain.cpp src/engine/terrain_layers.cpp
               src/engine/grass.cpp)
target_include_directories(terrain_preview PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(terrain_preview PRIVATE glm::glm)
target_compile_options(terrain_preview PRIVATE ${UKIYO_WARNINGS})
# Virtual geometry (docs/VGEOM.md). The builder and the selector are CPU code, so this test needs no device and runs
# anywhere, including CI.
# The docs figure for docs/VGEOM.md, rendered without a GPU: the same build and the same cuts the renderer uses, with a
# flat software rasteriser on top. CI compiles it so the tool cannot rot.
add_executable(vgeom_preview tools/vgeom_preview.cpp src/engine/terrain.cpp src/engine/vgeom.cpp
               third_party/meshoptimizer/simplifier.cpp third_party/meshoptimizer/allocator.cpp)
target_include_directories(vgeom_preview PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party
                           ${CMAKE_SOURCE_DIR}/third_party/meshoptimizer)
target_link_libraries(vgeom_preview PRIVATE glm::glm)
target_compile_options(vgeom_preview PRIVATE ${UKIYO_WARNINGS})

# The figure for docs/PLAYER.md: the shipping HUD layout, composited over an engine frame by a software rasteriser. It
# links only the CPU HUD code and STB, so CI compiles and runs it without a GPU.
add_executable(hud_preview tools/hud_preview.cpp src/engine/text_atlas.cpp src/engine/msdf.cpp src/engine/hud.cpp
               src/player/hud_layout.cpp)
target_include_directories(hud_preview PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(hud_preview PRIVATE glm::glm)
target_compile_options(hud_preview PRIVATE ${UKIYO_WARNINGS})

# The docs figure for docs/CHARACTER.md: a real CharacterVirtual walked across a real course by the shipping controller,
# drawn from the CharacterState it reports - the outline of each frame, the feet path, and the jump. No GPU, so CI
# renders it on every push and the figure cannot drift away from the code.
add_executable(character_preview tools/character_preview.cpp src/engine/text_atlas.cpp src/engine/msdf.cpp)
target_link_libraries(character_preview PRIVATE ukiyo_systems)
target_compile_options(character_preview PRIVATE ${UKIYO_WARNINGS})

# The docs figure for docs/LIGHTS.md: three authored lights on a three-sphere studio, shaded with the shipping
# packSceneLights and mtoon.frag's ramp, falloff and cone. No GPU, so CI renders it on every push.
add_executable(lights_preview tools/lights_preview.cpp src/engine/lights.cpp src/engine/text_atlas.cpp
               src/engine/msdf.cpp)
target_include_directories(lights_preview PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(lights_preview PRIVATE glm::glm)
target_compile_options(lights_preview PRIVATE ${UKIYO_WARNINGS})

# The CPU path tracer behind docs/PATHTRACE.md and docs/pathtrace-terrain.png: the ground-truth render of the Terrain
# Lab scene, on any CPU and with no GPU at all. STB is implemented in the tool itself, so it does not link ukiyo_core.
add_executable(pathtrace_preview tools/pathtrace_preview.cpp src/engine/terrain.cpp)
target_include_directories(pathtrace_preview PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(pathtrace_preview PRIVATE glm::glm)
target_compile_options(pathtrace_preview PRIVATE ${UKIYO_WARNINGS})

# Motion matching (docs/MOTION.md). The database, the BVH reader and the player are CPU code; the model retarget is
# compiled here too, so the test links the engine's model header without needing the renderer.
add_executable(motion_test tests/motion_test.cpp src/engine/motion_match.cpp)
target_include_directories(motion_test PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(motion_test PRIVATE glm::glm)
target_compile_options(motion_test PRIVATE ${UKIYO_WARNINGS})

# The real-time GI estimator's algorithm on the CPU (docs/PATHTRACE.md §4). The pass is GLSL and CI has no GPU, so the
# march, the hemisphere model and the accumulation weight are mirrored here and checked against analytic answers: the
# intersection of a ray with a plane, the identity that makes the composite a delta instead of a double-counted sky,
# and the occlusion sign inside a shaft.
add_executable(gi_test tests/gi_test.cpp)
target_include_directories(gi_test PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(gi_test PRIVATE glm::glm)
target_compile_options(gi_test PRIVATE ${UKIYO_WARNINGS})

# Trails (docs/TRAIL.md). The ribbon is CPU code with no device, so the width curve, the gradient, the trimming rules
# and the wound triangles are checked on any machine, including CI.
add_executable(trail_test tests/trail_test.cpp src/engine/trail.cpp)
target_include_directories(trail_test PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(trail_test PRIVATE glm::glm)
target_compile_options(trail_test PRIVATE ${UKIYO_WARNINGS})

# The figure in docs/TRAIL.md, rasterised on the CPU from the same ribbon triangles the renderer uploads.
add_executable(trail_preview tools/trail_preview.cpp src/engine/trail.cpp)
target_include_directories(trail_preview PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(trail_preview PRIVATE glm::glm)
target_compile_options(trail_preview PRIVATE ${UKIYO_WARNINGS})

# Multi-bounce ambient occlusion (docs/AO.md): the curve effects.frag evaluates, and its properties, on the CPU.
add_executable(ao_test tests/ao_test.cpp)
target_include_directories(ao_test PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(ao_test PRIVATE glm::glm)
target_compile_options(ao_test PRIVATE ${UKIYO_WARNINGS})

# The figure in docs/AO.md: a corner shaded four ways, including a two-bounce path trace as the reference.
add_executable(ao_preview tools/ao_preview.cpp)
target_include_directories(ao_preview PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(ao_preview PRIVATE glm::glm)
target_compile_options(ao_preview PRIVATE ${UKIYO_WARNINGS})

# Screen-space contact shadows (docs/CONTACT_SHADOWS.md): the march effects.frag runs, checked on an analytic scene.
add_executable(contact_shadow_test tests/contact_shadow_test.cpp)
target_include_directories(contact_shadow_test PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(contact_shadow_test PRIVATE glm::glm)
target_compile_options(contact_shadow_test PRIVATE ${UKIYO_WARNINGS})

# The figure in docs/CONTACT_SHADOWS.md: a shadow map that cannot resolve the contact, the march, and both.
add_executable(contact_shadow_preview tools/contact_shadow_preview.cpp)
target_include_directories(contact_shadow_preview PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(contact_shadow_preview PRIVATE glm::glm)
target_compile_options(contact_shadow_preview PRIVATE ${UKIYO_WARNINGS})

# Percentage-closer soft shadows (docs/PCSS.md): penumbra width from the light's size, the blocker's distance and the
# receiver's, shaped like the two-pass loop shaders/shadow_sample.glsl runs on the shadow map.
add_executable(pcss_test tests/pcss_test.cpp)
target_include_directories(pcss_test PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(pcss_test PRIVATE glm::glm)
target_compile_options(pcss_test PRIVATE ${UKIYO_WARNINGS})

# Multi-channel signed distance fields (docs/MSDF.md): the generator, the median reconstruction and the atlas against
# the exact outline, and against a rasterisation of each size with that rasteriser's own quarter-pixel sensitivity as
# the bar.
add_executable(msdf_test tests/msdf_test.cpp src/engine/msdf.cpp src/engine/text_atlas.cpp)
target_include_directories(msdf_test PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(msdf_test PRIVATE glm::glm)
target_compile_options(msdf_test PRIVATE ${UKIYO_WARNINGS})
target_compile_definitions(msdf_test PRIVATE UKIYO_ROOT="${CMAKE_SOURCE_DIR}")

# The figure in docs/WIREFRAME.md: one procedural scene drawn shaded and drawn as the edges of the same triangles, with
# hidden lines removed by the scene's own depth buffer.
add_executable(wireframe_preview tools/wireframe_preview.cpp)
target_include_directories(wireframe_preview PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party
                             ${CMAKE_SOURCE_DIR}/tools)
target_link_libraries(wireframe_preview PRIVATE glm::glm)
target_compile_options(wireframe_preview PRIVATE ${UKIYO_WARNINGS})

# The figure in docs/ASYNC_LOADING.md: what each frame pays while a scene loads, from the shipping scheduler with a fake
# clock, so the bars are the numbers the code produces.
add_executable(async_load_preview tools/async_load_preview.cpp src/engine/async_load.cpp)
target_include_directories(async_load_preview PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party
                             ${CMAKE_SOURCE_DIR}/tools)
target_link_libraries(async_load_preview PRIVATE glm::glm)
target_compile_options(async_load_preview PRIVATE ${UKIYO_WARNINGS})

# The figure in docs/MSDF.md: the shipping canvas laid out over a coverage page and a field page, drawn at sizes neither
# was made for, and the measurements printed on the sheet.
add_executable(msdf_preview tools/msdf_preview.cpp src/engine/msdf.cpp src/engine/text_atlas.cpp src/engine/hud.cpp)
target_include_directories(msdf_preview PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party
                             ${CMAKE_SOURCE_DIR}/tools)
target_link_libraries(msdf_preview PRIVATE glm::glm)
target_compile_options(msdf_preview PRIVATE ${UKIYO_WARNINGS})

# The figure in docs/PCSS.md: the same scene under a hard map, a swept fixed-radius PCF and PCSS, against 64-ray truth.
add_executable(pcss_preview tools/pcss_preview.cpp)
target_include_directories(pcss_preview PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party ${CMAKE_SOURCE_DIR}/tools)
target_link_libraries(pcss_preview PRIVATE glm::glm)
target_compile_options(pcss_preview PRIVATE ${UKIYO_WARNINGS})

# Alpha-coverage-preserving mipmaps (docs/ALPHA_MIPS.md): the chain builder the model loader runs, on the CPU.
add_executable(alpha_mips_test tests/alpha_mips_test.cpp src/engine/texture_mips.cpp)
target_include_directories(alpha_mips_test PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(alpha_mips_test PRIVATE glm::glm)
target_compile_options(alpha_mips_test PRIVATE ${UKIYO_WARNINGS})

# The figure in docs/ALPHA_MIPS.md: the plain chain against the preserved one, per mip level.
add_executable(alpha_mips_preview tools/alpha_mips_preview.cpp src/engine/texture_mips.cpp)
target_include_directories(alpha_mips_preview PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(alpha_mips_preview PRIVATE glm::glm)
target_compile_options(alpha_mips_preview PRIVATE ${UKIYO_WARNINGS})

# Fuzzy asset search and glob matching (docs/SEARCH.md): the engine half of the editor's Project filter. Pure C++17.
add_executable(search_test tests/search_test.cpp src/engine/search.cpp)
target_include_directories(search_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(search_test PRIVATE ${UKIYO_WARNINGS})

# The Project browser's thumbnail ladder (docs/FILESYSTEM.md): the size the slider, Ctrl + wheel and the grid share.
add_executable(browser_zoom_test tests/browser_zoom_test.cpp src/engine/browser_zoom.cpp)
target_include_directories(browser_zoom_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(browser_zoom_test PRIVATE ${UKIYO_WARNINGS})

# The figure in docs/FILESYSTEM.md: the zoom ladder, and glob queries run against a real asset list.
add_executable(browser_qol_preview tools/browser_qol_preview.cpp src/engine/browser_zoom.cpp src/engine/search.cpp)
target_include_directories(browser_qol_preview PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(browser_qol_preview PRIVATE glm::glm)
target_compile_options(browser_qol_preview PRIVATE ${UKIYO_WARNINGS})

# ---- Input system (docs/INPUT.md) ------------------------------------------
# The runtime and the .inputactions asset format are plain C++ (glm + nlohmann), with no GLFW, Vulkan or window, so the
# test below runs on any machine and in CI. The GLFW backend is its own target: only applications link it.
add_library(ukiyo_input STATIC ${UKIYO_INPUT_SOURCES})
target_include_directories(ukiyo_input PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ukiyo_input PUBLIC glm::glm nlohmann_json::nlohmann_json)
target_compile_options(ukiyo_input PRIVATE ${UKIYO_WARNINGS})

add_library(ukiyo_input_glfw STATIC src/input/input_glfw.cpp)
target_link_libraries(ukiyo_input_glfw PUBLIC ukiyo_input glfw)
target_compile_options(ukiyo_input_glfw PRIVATE ${UKIYO_WARNINGS})

add_executable(input_test tests/input_test.cpp)
target_link_libraries(input_test PRIVATE ukiyo_input)
target_compile_definitions(input_test PRIVATE UKIYO_ROOT="${CMAKE_SOURCE_DIR}")
target_compile_options(input_test PRIVATE ${UKIYO_WARNINGS})

# The motion demo (docs/MOTION.md): drives the controller through a scripted route with synthetic clips and writes
# the figure, so the runtime half of motion matching is exercised on every build.
add_executable(motion_demo tools/motion_demo.cpp src/engine/motion_match.cpp)
target_include_directories(motion_demo PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(motion_demo PRIVATE glm::glm)
target_compile_options(motion_demo PRIVATE ${UKIYO_WARNINGS})

add_executable(vgeom_test tests/vgeom_test.cpp src/engine/vgeom.cpp third_party/meshoptimizer/simplifier.cpp
               third_party/meshoptimizer/allocator.cpp)
target_include_directories(vgeom_test PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party
                           ${CMAKE_SOURCE_DIR}/third_party/meshoptimizer)
target_link_libraries(vgeom_test PRIVATE glm::glm)
target_compile_options(vgeom_test PRIVATE ${UKIYO_WARNINGS})

add_executable(particles_gpu_test tests/particles_gpu_test.cpp)
# The wireframe view (Flax 1.13): a quad drawn filled and drawn as its edges, on both backends, counted from the capture.
add_executable(wireframe_test tests/wireframe_test.cpp)
target_link_libraries(wireframe_test PRIVATE ukiyo_core)
target_compile_options(wireframe_test PRIVATE ${UKIYO_WARNINGS})
add_executable(water_render tests/water_render.cpp)
target_link_libraries(water_render PRIVATE ukiyo_core)
target_compile_options(water_render PRIVATE ${UKIYO_WARNINGS})
target_link_libraries(particles_gpu_test PRIVATE ukiyo_core)
target_compile_options(particles_gpu_test PRIVATE ${UKIYO_WARNINGS})
target_link_libraries(systems_test PRIVATE ukiyo_systems)
target_compile_definitions(systems_test PRIVATE UKIYO_ROOT="${CMAKE_SOURCE_DIR}")
target_compile_options(systems_test PRIVATE ${UKIYO_WARNINGS})

add_executable(network_test tests/network_test.cpp)
target_link_libraries(network_test PRIVATE ukiyo_systems)
target_compile_options(network_test PRIVATE ${UKIYO_WARNINGS})

# Project templates and the hub's new-project scaffold. No Qt, so this runs anywhere.
add_executable(project_test tests/project_test.cpp)
target_link_libraries(project_test PRIVATE ukiyo_core)
target_compile_definitions(project_test PRIVATE UKIYO_ROOT="${CMAKE_SOURCE_DIR}")
target_compile_options(project_test PRIVATE ${UKIYO_WARNINGS})

# ---- Player -----------------------------------------------------------------
# The shipped game: manifest, scenes, World, HUD. It is deliberately thin, because everything it does beyond opening a
# window lives in PlayerApp, which the player_test covers without a device.
add_executable(ukiyo_player src/player/main.cpp src/player/player_app.cpp)
target_link_libraries(ukiyo_player PRIVATE ukiyo_systems ukiyo_core)
target_include_directories(ukiyo_player PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
# glfw3.h includes GL/gl.h unless told not to; the player never makes a GL call itself.
target_compile_definitions(ukiyo_player PRIVATE GLFW_INCLUDE_NONE)
target_compile_options(ukiyo_player PRIVATE ${UKIYO_WARNINGS})

# ---- Application -----------------------------------------------------------
add_executable(ukiyo src/app/main.cpp)
target_link_libraries(ukiyo PRIVATE ukiyo_core ukiyo_input_glfw)
# glfw3.h includes GL/gl.h unless told not to. The viewer never makes a GL call itself (the RHI does), so it asks GLFW
# for the no-GL header and the build does not need GL development headers on the machine.
target_compile_definitions(ukiyo PRIVATE GLFW_INCLUDE_NONE UKIYO_ROOT="${CMAKE_SOURCE_DIR}")
target_compile_options(ukiyo PRIVATE ${UKIYO_WARNINGS})

# ---- Tests -----------------------------------------------------------------
# Scene transforms and the instance hierarchy (docs/PLAYER.md).
add_executable(transform_test tests/transform_test.cpp src/engine/transform.cpp)
target_include_directories(transform_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(transform_test PRIVATE glm::glm)
target_compile_options(transform_test PRIVATE ${UKIYO_WARNINGS_PEDANTIC})

# Scene lights and the GPU packing (docs/PLAYER.md).
add_executable(lights_test tests/lights_test.cpp src/engine/lights.cpp)
target_include_directories(lights_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lights_test PRIVATE glm::glm)
target_compile_options(lights_test PRIVATE ${UKIYO_WARNINGS_PEDANTIC})

# The save format (docs/PLAYER.md).
add_executable(save_test tests/save_test.cpp src/systems/save_game.cpp)
target_include_directories(save_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(save_test PRIVATE glm::glm)
target_compile_options(save_test PRIVATE ${UKIYO_WARNINGS_PEDANTIC})

# The Inspector's search rule (Flax 1.13's Properties panel search): one query over a page of rows, which rows stay and
# which sections survive. Pure C++ over strings, so it runs anywhere.
add_executable(property_search_test tests/property_search_test.cpp src/engine/property_search.cpp src/engine/search.cpp)
target_include_directories(property_search_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(property_search_test PRIVATE ${UKIYO_WARNINGS_PEDANTIC})

# Undo as deltas (docs/EDITOR.md): diff, apply and invert over RFC 6901 pointers. The editor's undo stack stores one of
# these per edit instead of a copy of the whole scene.
add_executable(json_patch_test tests/json_patch_test.cpp src/engine/json_patch.cpp)
target_include_directories(json_patch_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(json_patch_test PRIVATE nlohmann_json::nlohmann_json)
target_compile_options(json_patch_test PRIVATE ${UKIYO_WARNINGS_PEDANTIC})

# The asset pipeline: .meta sidecars, stable GUIDs, the guid -> path index, broken references, a move repaired by GUID,
# and the scene entry an import setting produces.
add_executable(asset_db_test tests/asset_db_test.cpp src/engine/asset_db.cpp)
target_include_directories(asset_db_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(asset_db_test PRIVATE nlohmann_json::nlohmann_json)
target_compile_options(asset_db_test PRIVATE ${UKIYO_WARNINGS_PEDANTIC})

# The other editor tools: git status parsing, git command lines, the locale files, and the packer's argument list.
add_executable(editor_tools_test tests/editor_tools_test.cpp src/engine/vcs.cpp src/engine/localisation.cpp
               src/pack/pack_cli.cpp)
target_include_directories(editor_tools_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(editor_tools_test PRIVATE nlohmann_json::nlohmann_json)
target_compile_options(editor_tools_test PRIVATE ${UKIYO_WARNINGS_PEDANTIC})

# The plugin ABI, against real shared libraries. CMake builds two plugins with the C compiler and passes their paths
# in, so the test exercises dlopen/LoadLibrary, the exported symbols and the table a plugin calls back through - the
# three places a plugin API is wrong in a way a mock would never show.
add_library(plugin_sample SHARED tests/plugin_sample.c)
target_include_directories(plugin_sample PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_target_properties(plugin_sample PROPERTIES PREFIX "" C_STANDARD 11)
add_library(plugin_refuse SHARED tests/plugin_refuse.c)
target_include_directories(plugin_refuse PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_target_properties(plugin_refuse PROPERTIES PREFIX "" C_STANDARD 11)
add_executable(plugin_test tests/plugin_test.cpp src/engine/plugin_host.cpp)
target_include_directories(plugin_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(plugin_test PRIVATE ${CMAKE_DL_LIBS})
target_compile_options(plugin_test PRIVATE ${UKIYO_WARNINGS_PEDANTIC})
add_dependencies(plugin_test plugin_sample plugin_refuse)
# The paths are baked in rather than passed by a test runner: this project has no CTest convention, and CI runs the
# executables by name.
target_compile_definitions(plugin_test PRIVATE
  UKIYO_PLUGIN_SAMPLE="$<TARGET_FILE:plugin_sample>"
  UKIYO_PLUGIN_REFUSE="$<TARGET_FILE:plugin_refuse>")

# The camera the Scene view builds with: perspective, orthographic, zoom, pan and flying.
# The Scene view's gizmos: only the geometry, so it runs without a window. Pointing, plane and ring drags, Blender's
# visibility rule, snapping, and the view gizmo's layout.
add_executable(gizmo_math_test tests/gizmo_math_test.cpp src/engine/gizmo_math.cpp src/engine/transform.cpp)
target_include_directories(gizmo_math_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gizmo_math_test PRIVATE glm::glm)
target_compile_options(gizmo_math_test PRIVATE ${UKIYO_WARNINGS_PEDANTIC})

# The figure in docs/EDITOR.md: the three transform tools and the corner view widget, drawn with the geometry above, so
# the figure moves when the geometry does and the tool stops building when that API does.
# transform.cpp with gizmo_math.cpp: the selection maths calls the engine's Euler/quaternion pair, and the figure
# is built from the same translation units the editor uses, so it cannot drift from them.
add_executable(gizmo_preview tools/gizmo_preview.cpp src/engine/gizmo_math.cpp src/engine/transform.cpp
               src/engine/camera.cpp)
target_include_directories(gizmo_preview PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(gizmo_preview PRIVATE glm::glm)
target_compile_options(gizmo_preview PRIVATE ${UKIYO_WARNINGS})

# The launcher icon, in five densities, from the identity's own drawing. Committed PNGs, regenerable: CI reruns the
# tool and fails when the committed files and what the tool writes have drifted.
add_executable(android_icon tools/android_icon.cpp)
target_include_directories(android_icon PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
target_compile_options(android_icon PRIVATE ${UKIYO_WARNINGS})

add_executable(camera_test tests/camera_test.cpp src/engine/camera.cpp)
target_include_directories(camera_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(camera_test PRIVATE glm::glm)
target_compile_options(camera_test PRIVATE ${UKIYO_WARNINGS_PEDANTIC})

# Budgeted loading (Flax 1.11's async scene loading): the scheduler that decides how much of a scene one frame pays for,
# driven by a fake clock so a budget is a number the test can hold it to.
add_executable(async_load_test tests/async_load_test.cpp src/engine/async_load.cpp)
target_include_directories(async_load_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(async_load_test PRIVATE ${UKIYO_WARNINGS_PEDANTIC})

# HUD text and layout. Needs the repository root for the test font.
add_executable(hud_test tests/hud_test.cpp src/engine/text_atlas.cpp src/engine/msdf.cpp src/engine/hud.cpp)
target_include_directories(hud_test PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_compile_definitions(hud_test PRIVATE UKIYO_ROOT="${CMAKE_SOURCE_DIR}")
target_link_libraries(hud_test PRIVATE glm::glm)
target_compile_options(hud_test PRIVATE ${UKIYO_WARNINGS})

# The build manifest: the contract between the editor's Build page and the player.
add_executable(manifest_test tests/manifest_test.cpp src/engine/build_manifest.cpp)
target_include_directories(manifest_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(manifest_test PRIVATE glm::glm nlohmann_json::nlohmann_json)
target_compile_options(manifest_test PRIVATE ${UKIYO_WARNINGS})

# The runtime scene reader.
add_executable(scene_runtime_test tests/scene_runtime_test.cpp src/engine/scene_runtime.cpp
               src/engine/transform.cpp src/engine/lights.cpp)
target_include_directories(scene_runtime_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(scene_runtime_test PRIVATE glm::glm nlohmann_json::nlohmann_json)
target_compile_options(scene_runtime_test PRIVATE ${UKIYO_WARNINGS})

# The capsule character (item 4), through the situations that define a controller: landing, walking, a wall, a step, a
# slope too steep to climb, a jump, and two characters that must not share state.
add_executable(character_test tests/character_test.cpp)
target_link_libraries(character_test PRIVATE ukiyo_systems)
target_compile_options(character_test PRIVATE ${UKIYO_WARNINGS})

# The player's own logic, with the real World, physics, Lua and audio (device-free): the whole boot, tick and scene
# switch path, on a temporary project.
add_executable(player_test tests/player_test.cpp src/player/player_app.cpp)
target_link_libraries(player_test PRIVATE ukiyo_systems ukiyo_core)
target_compile_definitions(player_test PRIVATE GLFW_INCLUDE_NONE)
target_compile_options(player_test PRIVATE ${UKIYO_WARNINGS})

# The first game (docs/GAME.md): Lantern Run's project, scene and generated assets, checked without a device. A broken
# model path or a scene that no longer loads fails here, not on a player's machine. game_data_test is the portable half
# of the same audit, built from the translation units the Android job compiles.
add_executable(game_data_test tests/game_data_test.cpp src/engine/build_manifest.cpp
               src/engine/scene_runtime.cpp src/engine/transform.cpp src/engine/lights.cpp)
target_include_directories(game_data_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(game_data_test PRIVATE glm::glm nlohmann_json::nlohmann_json)
target_compile_options(game_data_test PRIVATE ${UKIYO_WARNINGS})

add_executable(game_test tests/game_test.cpp)
target_link_libraries(game_test PRIVATE ukiyo_core)
target_compile_options(game_test PRIVATE ${UKIYO_WARNINGS})

# Afterglow (docs/GAME.md): the second game's contract — manifest, scene, the ten relights the script and the
# scene must agree on, the locales, and the audio tools/audio_gen.py wrote. Like game_data_test it needs only the
# readers, so it builds in the Android job's ABI loop too.
add_executable(afterglow_test tests/afterglow_test.cpp src/engine/build_manifest.cpp
               src/engine/scene_runtime.cpp src/engine/transform.cpp src/engine/lights.cpp
               src/engine/localisation.cpp)
target_include_directories(afterglow_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(afterglow_test PRIVATE glm::glm nlohmann_json::nlohmann_json)
target_compile_options(afterglow_test PRIVATE ${UKIYO_WARNINGS})

# The packer (docs/PACKAGING.md): what a package contains, that it verifies, and that a corrupted file is caught.
add_executable(pack_test tests/pack_test.cpp)
target_link_libraries(pack_test PRIVATE ukiyo_pack_core)
target_compile_options(pack_test PRIVATE ${UKIYO_WARNINGS})

add_executable(lod_gpu_test tests/lod_gpu_test.cpp)
target_link_libraries(lod_gpu_test PRIVATE ukiyo_core)
target_compile_options(lod_gpu_test PRIVATE ${UKIYO_WARNINGS})

add_executable(endfield_shader_test tests/endfield_shader_test.cpp)
target_link_libraries(endfield_shader_test PRIVATE ukiyo_core)
target_compile_definitions(endfield_shader_test PRIVATE UKIYO_ROOT="${CMAKE_SOURCE_DIR}")
target_compile_options(endfield_shader_test PRIVATE ${UKIYO_WARNINGS})

add_executable(endfield_gpu_test tests/endfield_gpu_test.cpp)
target_link_libraries(endfield_gpu_test PRIVATE ukiyo_core)
target_compile_options(endfield_gpu_test PRIVATE ${UKIYO_WARNINGS})

add_executable(endfield_scene_test tests/endfield_scene_test.cpp)
target_link_libraries(endfield_scene_test PRIVATE ukiyo_core)
target_compile_options(endfield_scene_test PRIVATE ${UKIYO_WARNINGS})

add_executable(rig_smoke tests/rig_smoke.cpp)
target_link_libraries(rig_smoke PRIVATE ukiyo_core)
target_compile_options(rig_smoke PRIVATE ${UKIYO_WARNINGS})

# ---- World editor (Qt6 Widgets) ---------------------------------------------
option(UKIYO_EDITOR "Build the Ukiyo World Editor (needs Qt6 Widgets)" ON)
if(UKIYO_EDITOR)
  find_package(Qt6 COMPONENTS Widgets OpenGL QUIET)
  if(Qt6_FOUND)
    # Node canvas for the shader graph: QtNodes (BSD-3-Clause), vendored under third_party/nodeeditor.
    file(GLOB UKIYO_NODE_SRC ${CMAKE_SOURCE_DIR}/third_party/nodeeditor/src/*.cpp)
    file(GLOB UKIYO_NODE_HDR ${CMAKE_SOURCE_DIR}/third_party/nodeeditor/include/QtNodes/internal/*.hpp)
    add_library(ukiyo_nodeeditor STATIC ${UKIYO_NODE_SRC} ${UKIYO_NODE_HDR}
      ${CMAKE_SOURCE_DIR}/third_party/nodeeditor/resources/resources.qrc)
    set_target_properties(ukiyo_nodeeditor PROPERTIES AUTOMOC ON AUTORCC ON CXX_STANDARD 17 POSITION_INDEPENDENT_CODE ON)
    target_include_directories(ukiyo_nodeeditor PUBLIC ${CMAKE_SOURCE_DIR}/third_party/nodeeditor/include)
    target_include_directories(ukiyo_nodeeditor PRIVATE ${CMAKE_SOURCE_DIR}/third_party/nodeeditor/src
      ${CMAKE_SOURCE_DIR}/third_party/nodeeditor/include/QtNodes/internal)
    target_compile_definitions(ukiyo_nodeeditor PUBLIC NODE_EDITOR_STATIC)
    target_link_libraries(ukiyo_nodeeditor PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL)
    target_compile_options(ukiyo_nodeeditor PRIVATE ${UKIYO_NO_WARNINGS})

    # Shader tool: default shader graphs, node coverage, and .ukshader round-trip checks.
    add_executable(ukiyo_shader_tool tools/ukiyo_shader_tool.cpp
      editor/src/shader_graph/sg_nodes.cpp editor/src/shader_graph/sg_codegen.cpp
      editor/src/shader_graph/sg_nodes.h editor/src/shader_graph/sg_codegen.h)
    set_target_properties(ukiyo_shader_tool PROPERTIES AUTOMOC ON CXX_STANDARD 17)
    target_include_directories(ukiyo_shader_tool PRIVATE ${CMAKE_SOURCE_DIR}/editor/src/shader_graph)
    target_link_libraries(ukiyo_shader_tool PRIVATE ukiyo_core ukiyo_nodeeditor Qt6::Widgets)
    target_compile_options(ukiyo_shader_tool PRIVATE ${UKIYO_WARNINGS})

    add_executable(ukiyo_editor
      editor/src/main.cpp
      editor/src/splash_screen.cpp
      editor/src/splash_screen.h
      editor/src/engine_session.cpp
      editor/src/render_worker.cpp
      editor/src/prefab_file.cpp
    editor/src/systems_json.cpp
    editor/src/scene_file.cpp
    editor/src/scene_gizmo.cpp
    editor/src/view_gizmo.cpp
    editor/src/asset_ops.cpp
    editor/src/anim_file.cpp
    editor/src/anim_graph.cpp
    editor/src/anim_panel.cpp
    editor/src/anim_timeline.cpp
      editor/src/icons.cpp
      editor/src/asset_preview.cpp
      editor/src/material_thumbnail.cpp
      editor/src/material_inspector.cpp
      editor/src/prefs_dialog.cpp
      editor/src/lua_editor.cpp
      editor/src/lua_editor.h
      editor/src/scripting_tools.cpp
      editor/src/scripting_tools.h
      editor/src/shader_graph/sg_nodes.cpp
      editor/src/shader_graph/sg_codegen.cpp
      editor/src/shader_graph/shader_graph_window.cpp
      editor/src/main_window.cpp
      editor/src/main_window_terrain.cpp
      editor/src/shader_graph/sg_nodes.h
      editor/src/shader_graph/sg_codegen.h
      editor/src/shader_graph/shader_graph_window.h
      editor/src/engine_session.h
      editor/src/render_worker.h
      editor/src/icons.h
      editor/src/asset_preview.h
      editor/src/prefs_dialog.h
      editor/src/material_icons.h
      editor/src/main_window.h
      editor/src/inspector_filter.cpp
      editor/src/inspector_filter.h
      editor/src/foldout.h
      editor/src/inspector_ui.h)
    set_target_properties(ukiyo_editor PROPERTIES AUTOMOC ON CXX_STANDARD 17)
    target_include_directories(ukiyo_editor PRIVATE ${CMAKE_SOURCE_DIR}/editor/src)
    target_compile_definitions(ukiyo_editor PRIVATE UKIYO_ROOT="${CMAKE_SOURCE_DIR}")
    # ukiyo_pack_core is the package command: the Build page calls the same argument builder the packer and its test do.
    target_link_libraries(ukiyo_editor PRIVATE ukiyo_core ukiyo_systems ukiyo_pack_core ukiyo_nodeeditor Qt6::Widgets)
    target_compile_options(ukiyo_editor PRIVATE ${UKIYO_WARNINGS})
    # main_window.cpp is one translation unit of several tens of thousands of lines, and it is now past the point where
    # MSVC can put all of its symbols in the default number of COFF sections: C1128, "number of sections exceeded object
    # file format limit". /bigobj raises that limit; it changes nothing else about the object file.
    if(MSVC)
      target_compile_options(ukiyo_editor PRIVATE /bigobj)
    endif()
    # main_window.cpp is very large. Optimising it at -O3 needs more memory than the build machine has,
    # so this one file is built at -O0 (the build machine has about 1 GB; -O1 runs out of memory here).
    # The rest of the editor keeps the Release flags. MSVC has no -O0; /Od is its equivalent and its optimiser copes
    # with the file at /O2, so Windows keeps the default there.
    if(NOT MSVC)
      set_source_files_properties(${CMAKE_SOURCE_DIR}/editor/src/main_window.cpp PROPERTIES COMPILE_OPTIONS "-O0")
      set_source_files_properties(${CMAKE_SOURCE_DIR}/editor/src/main_window_terrain.cpp PROPERTIES COMPILE_OPTIONS "-O0")
    endif()

    # The hub: a launcher that lists recent projects, scaffolds new ones from the templates, and
    # starts the editor on them with --project.
    add_executable(ukiyo_hub
      editor/src/hub/hub_main.cpp
      editor/src/hub/hub_window.cpp
      editor/src/hub/hub_window.h
      editor/src/hub/new_project_dialog.cpp
      editor/src/hub/new_project_dialog.h
      editor/src/hub/project_launcher.cpp
      editor/src/hub/project_launcher.h
      editor/src/hub/hub_selftest.cpp
      editor/src/hub/hub_selftest.h
      editor/src/splash_screen.cpp
      editor/src/splash_screen.h
      editor/src/icons.cpp)
    set_target_properties(ukiyo_hub PROPERTIES AUTOMOC ON CXX_STANDARD 17)
    target_include_directories(ukiyo_hub PRIVATE ${CMAKE_SOURCE_DIR}/editor/src)
    target_compile_definitions(ukiyo_hub PRIVATE UKIYO_ROOT="${CMAKE_SOURCE_DIR}")
    target_link_libraries(ukiyo_hub PRIVATE ukiyo_core Qt6::Widgets)
    target_compile_options(ukiyo_hub PRIVATE ${UKIYO_WARNINGS})
  else()
    message(STATUS "Qt6 Widgets not found: ukiyo_editor is skipped")
  endif()
endif()
