cmake_minimum_required(VERSION 3.20)
project(FightReview VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(FR_WARNINGS /W4 /permissive- /utf-8)

# ImGui must match the version Nexus renders with (Raidcore fork, 1.80).
add_library(imgui STATIC
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
)
target_include_directories(imgui PUBLIC external/imgui)

# miniz (MIT) for the zip around .zevtc logs
add_library(miniz STATIC external/miniz/miniz.c)
target_include_directories(miniz PUBLIC external)
target_compile_definitions(miniz PUBLIC MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ARCHIVE_WRITING_APIS
    MINIZ_NO_ZLIB_COMPATIBLE_NAMES)

# Game-independent: log reader and fight analysis
add_library(fr_core STATIC
    src/Evtc.cpp
    src/Analysis.cpp
    src/InstantCasts.cpp
)
target_include_directories(fr_core PUBLIC src external)
target_compile_options(fr_core PRIVATE ${FR_WARNINGS})
target_link_libraries(fr_core PUBLIC miniz)

add_library(FightReview SHARED
    src/entry.cpp
    src/Icons.cpp
    src/Session.cpp
    src/SkillIcons.cpp
    src/Ui.cpp src/UiCommon.cpp src/UiReview.cpp src/UiCompare.cpp src/UiSquad.cpp src/UiFight.cpp
)
target_include_directories(FightReview PRIVATE src external)
target_compile_definitions(FightReview PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
    FR_VERSION_MAJOR=${PROJECT_VERSION_MAJOR} FR_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    FR_VERSION_PATCH=${PROJECT_VERSION_PATCH})
target_compile_options(FightReview PRIVATE ${FR_WARNINGS})
target_link_libraries(FightReview PRIVATE fr_core imgui winhttp)

# Prints the analysis of one log, for tools/check_cpp.py
add_executable(frcheck tools/frcheck.cpp)
target_compile_options(frcheck PRIVATE ${FR_WARNINGS})
target_link_libraries(frcheck PRIVATE fr_core)

# Offscreen render of the real window from real logs, with ImGui asserts on (tools/uishot)
add_library(imgui_checked STATIC
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/imgui/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui_checked PUBLIC external/imgui external tools/uishot)
target_compile_definitions(imgui_checked PUBLIC IMGUI_USER_CONFIG="uishot_config.h")
add_executable(fr_uishot tools/uishot/main.cpp src/Icons.cpp src/Session.cpp src/SkillIcons.cpp src/Ui.cpp src/UiCommon.cpp src/UiReview.cpp src/UiCompare.cpp src/UiSquad.cpp src/UiFight.cpp)
target_include_directories(fr_uishot PRIVATE src external tools/uishot)
target_compile_definitions(fr_uishot PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(fr_uishot PRIVATE fr_core imgui_checked d3d11 dxgi windowscodecs d3dcompiler winhttp)
