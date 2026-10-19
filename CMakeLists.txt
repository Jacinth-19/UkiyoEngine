cmake_minimum_required(VERSION 3.20)
project(UkiyoEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_package(Vulkan REQUIRED)
find_package(glfw3 REQUIRED)
find_package(glm REQUIRED)
find_package(nlohmann_json REQUIRED)
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
# glslang is linked into the engine: GLSL -> SPIR-V at runtime (generated material shaders).
find_package(glslang CONFIG REQUIRED)

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
target_compile_options(ukiyo_recast PRIVATE -w)

# Lua 5.4 (scripting). Core and standard libraries only; the standalone interpreter is not built.
file(GLOB LUA_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/third_party/lua/*.c)
add_library(ukiyo_lua STATIC ${LUA_SRC})
target_include_directories(ukiyo_lua PUBLIC ${CMAKE_SOURCE_DIR}/third_party/lua)
target_compile_definitions(ukiyo_lua PRIVATE LUA_USE_LINUX)
target_compile_options(ukiyo_lua PRIVATE -w)

# miniaudio (audio output, mixing, and 3D sound).
add_library(ukiyo_miniaudio STATIC ${CMAKE_SOURCE_DIR}/third_party/miniaudio/miniaudio_impl.cpp)
target_include_directories(ukiyo_miniaudio PUBLIC ${CMAKE_SOURCE_DIR}/third_party/miniaudio)
target_compile_options(ukiyo_miniaudio PRIVATE -w)
target_link_libraries(ukiyo_miniaudio PUBLIC ${CMAKE_DL_LIBS} pthread m)

# ---- Engine core library ---------------------------------------------------
add_library(ukiyo_core STATIC
  src/rhi/gl_device.cpp
  src/rhi/vk_device.cpp
  src/engine/anim.cpp
  src/engine/camera.cpp
  src/engine/face.cpp
  src/engine/model.cpp
  src/engine/pmx_glb.cpp
  src/engine/vfs.cpp
  src/engine/shader_compiler.cpp
  src/engine/material_asset.cpp
  src/engine/project_settings.cpp
  src/engine/project_template.cpp
  src/engine/graph_material.cpp
  src/engine/water.cpp
  src/engine/terrain.cpp
  src/engine/terrain_gpu.cpp
  src/engine/terrain_layers.cpp
  src/engine/foliage.cpp
  src/engine/terrain_edit.cpp
  third_party/meshoptimizer/simplifier.cpp
  third_party/meshoptimizer/allocator.cpp
  src/engine/material_preview.cpp
  src/engine/particle_renderer.cpp
  src/engine/renderer.cpp
  src/engine/rig.cpp
  src/third_party_impl.cpp
  third_party/glad/src/gl.c)
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
target_compile_options(ukiyo_core PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

# ---- Game systems: physics, navigation, audio, animation state, scripting, particles -----
add_library(ukiyo_systems STATIC
  src/systems/physics.cpp
  src/systems/navigation.cpp
  src/systems/audio.cpp
  src/systems/anim_state.cpp
  src/systems/script.cpp
  src/systems/vfx.cpp
  src/systems/network.cpp
  src/systems/world.cpp)
target_include_directories(ukiyo_systems PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ukiyo_systems PUBLIC ukiyo_core Jolt ukiyo_recast ukiyo_lua ukiyo_miniaudio)
if(WIN32)
  target_link_libraries(ukiyo_systems PUBLIC ws2_32)
endif()
target_compile_options(ukiyo_systems PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

add_executable(systems_test tests/systems_test.cpp)
add_executable(pmx_glb_test tests/pmx_glb_test.cpp src/engine/pmx_glb.cpp)
target_include_directories(pmx_glb_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(pmx_glb_test PRIVATE -Wall -Wextra -Wpedantic)
add_executable(ukiyo_pak tools/ukiyo_pak.cpp)
target_link_libraries(ukiyo_pak PRIVATE ukiyo_core)
add_executable(particles_gpu_test tests/particles_gpu_test.cpp)
add_executable(water_render tests/water_render.cpp)
target_link_libraries(water_render PRIVATE ukiyo_core)
target_compile_options(water_render PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
target_link_libraries(particles_gpu_test PRIVATE ukiyo_core)
target_compile_options(particles_gpu_test PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
target_link_libraries(systems_test PRIVATE ukiyo_systems)
target_compile_definitions(systems_test PRIVATE UKIYO_ROOT="${CMAKE_SOURCE_DIR}")
target_compile_options(systems_test PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

add_executable(network_test tests/network_test.cpp)
target_link_libraries(network_test PRIVATE ukiyo_systems)
target_compile_options(network_test PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

# Project templates and the hub's new-project scaffold. No Qt, so this runs anywhere.
add_executable(project_test tests/project_test.cpp)
target_link_libraries(project_test PRIVATE ukiyo_core)
target_compile_definitions(project_test PRIVATE UKIYO_ROOT="${CMAKE_SOURCE_DIR}")
target_compile_options(project_test PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

# ---- Application -----------------------------------------------------------
add_executable(ukiyo src/app/main.cpp)
target_link_libraries(ukiyo PRIVATE ukiyo_core)
target_compile_options(ukiyo PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

# ---- Tests -----------------------------------------------------------------
add_executable(lod_gpu_test tests/lod_gpu_test.cpp)
target_link_libraries(lod_gpu_test PRIVATE ukiyo_core)
target_compile_options(lod_gpu_test PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

add_executable(endfield_shader_test tests/endfield_shader_test.cpp)
target_link_libraries(endfield_shader_test PRIVATE ukiyo_core)
target_compile_definitions(endfield_shader_test PRIVATE UKIYO_ROOT="${CMAKE_SOURCE_DIR}")
target_compile_options(endfield_shader_test PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

add_executable(endfield_gpu_test tests/endfield_gpu_test.cpp)
target_link_libraries(endfield_gpu_test PRIVATE ukiyo_core)
target_compile_options(endfield_gpu_test PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

add_executable(endfield_scene_test tests/endfield_scene_test.cpp)
target_link_libraries(endfield_scene_test PRIVATE ukiyo_core)
target_compile_options(endfield_scene_test PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

add_executable(rig_smoke tests/rig_smoke.cpp)
target_link_libraries(rig_smoke PRIVATE ukiyo_core)
target_compile_options(rig_smoke PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

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
    target_compile_options(ukiyo_nodeeditor PRIVATE -w)

    # Shader tool: default shader graphs, node coverage, and .ukshader round-trip checks.
    add_executable(ukiyo_shader_tool tools/ukiyo_shader_tool.cpp
      editor/src/shader_graph/sg_nodes.cpp editor/src/shader_graph/sg_codegen.cpp
      editor/src/shader_graph/sg_nodes.h editor/src/shader_graph/sg_codegen.h)
    set_target_properties(ukiyo_shader_tool PROPERTIES AUTOMOC ON CXX_STANDARD 17)
    target_include_directories(ukiyo_shader_tool PRIVATE ${CMAKE_SOURCE_DIR}/editor/src/shader_graph)
    target_link_libraries(ukiyo_shader_tool PRIVATE ukiyo_core ukiyo_nodeeditor Qt6::Widgets)
    target_compile_options(ukiyo_shader_tool PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

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
      editor/src/inspector_ui.h)
    set_target_properties(ukiyo_editor PROPERTIES AUTOMOC ON CXX_STANDARD 17)
    target_include_directories(ukiyo_editor PRIVATE ${CMAKE_SOURCE_DIR}/editor/src)
    target_compile_definitions(ukiyo_editor PRIVATE UKIYO_ROOT="${CMAKE_SOURCE_DIR}")
    target_link_libraries(ukiyo_editor PRIVATE ukiyo_core ukiyo_systems ukiyo_nodeeditor Qt6::Widgets)
    target_compile_options(ukiyo_editor PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
    # main_window.cpp is very large. Optimising it at -O3 needs more memory than the build machine has,
    # so this one file is built at -O0 (the build machine has about 1 GB; -O1 runs out of memory here).
    # The rest of the editor keeps the Release flags.
    set_source_files_properties(${CMAKE_SOURCE_DIR}/editor/src/main_window.cpp PROPERTIES COMPILE_OPTIONS "-O0")
    set_source_files_properties(${CMAKE_SOURCE_DIR}/editor/src/main_window_terrain.cpp PROPERTIES COMPILE_OPTIONS "-O0")

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
    target_compile_options(ukiyo_hub PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
  else()
    message(STATUS "Qt6 Widgets not found: ukiyo_editor is skipped")
  endif()
endif()
