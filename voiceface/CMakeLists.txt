cmake_minimum_required(VERSION 3.16)
project(voiceface LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Standalone: this project does not link into the Ukiyo engine build.
add_library(voiceface_core STATIC
    src/viseme_mapper.cpp
    src/rig_controller.cpp
    src/sync_analyzer.cpp
    src/tone_voice_cloner.cpp)
target_include_directories(voiceface_core PUBLIC include)
target_compile_options(voiceface_core PRIVATE -Wall -Wextra)

add_executable(voiceface_sync src/voiceface_sync.cpp)
target_link_libraries(voiceface_sync PRIVATE voiceface_core)
target_compile_options(voiceface_sync PRIVATE -Wall -Wextra)

enable_testing()
add_executable(voiceface_test tests/voiceface_test.cpp)
target_link_libraries(voiceface_test PRIVATE voiceface_core)
target_compile_options(voiceface_test PRIVATE -Wall -Wextra)
add_test(NAME voiceface_test COMMAND voiceface_test)
