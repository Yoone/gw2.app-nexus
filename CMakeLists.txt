cmake_minimum_required(VERSION 3.21)

project(gw2app-nexus LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "This is the Windows MSVC build. On macOS or Linux, run make.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Guild Wars 2 is 64-bit. Configure with -A x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Pinned to the ImGui version Nexus links. We share the host's context, so a different
# version compiles cleanly and then corrupts memory at runtime.
set(IMGUI_SOURCES
    vendor/imgui/imgui.cpp
    vendor/imgui/imgui_draw.cpp
    vendor/imgui/imgui_tables.cpp
    vendor/imgui/imgui_widgets.cpp
)

file(GLOB_RECURSE ADDON_SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_library(gw2app SHARED
    ${ADDON_SOURCES}
    res/gw2app.rc
    ${IMGUI_SOURCES}
)

target_include_directories(gw2app PRIVATE
    src
    res
    vendor
    vendor/imgui
)

# ws2_32: the localhost server. shell32: opening URLs in the browser.
target_link_libraries(gw2app PRIVATE ws2_32 shell32)

target_compile_definitions(gw2app PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Nexus loads a plain "<name>.dll" from the addons folder, so drop CMake's "lib" prefix.
set_target_properties(gw2app PROPERTIES
    OUTPUT_NAME "gw2app"
    PREFIX ""
    SUFFIX ".dll"
)

# Static CRT, so users do not need the VC++ redistributable installed.
set_property(TARGET gw2app PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    target_compile_options(gw2app PRIVATE /W3 /permissive-)
    # ImGui is pinned, so its warnings are not ours to fix and would only bury our own.
    set_source_files_properties(${IMGUI_SOURCES} PROPERTIES COMPILE_OPTIONS "/w")
endif()
