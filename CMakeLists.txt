cmake_minimum_required(VERSION 3.20)
project(2009tool CXX)

# ── x86 guard ────────────────────────────────────────────────────────────────
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Configure for x86: cmake -A Win32  (or use the x86 toolset)")
endif()

# ── DirectX SDK ───────────────────────────────────────────────────────────────
set(DXSDK "C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)")
if(NOT EXISTS "${DXSDK}/Include/d3d9.h")
    message(FATAL_ERROR "DirectX SDK not found at ${DXSDK}")
endif()

# ── Shared compile options (match build.bat) ──────────────────────────────────
add_compile_options(/O2 /MT /EHa /W3 /D_CRT_SECURE_NO_WARNINGS)

# ── internal.dll ─────────────────────────────────────────────────────────────
add_library(internal SHARED
    src/dllmain.cpp
    src/hooks.cpp
    src/esp.cpp
    src/menu.cpp
    src/executor.cpp
    src/explorer.cpp
    vendor/TextEditor.cpp
    vendor/imgui/imgui.cpp
    vendor/imgui/imgui_draw.cpp
    vendor/imgui/imgui_tables.cpp
    vendor/imgui/imgui_widgets.cpp
    vendor/imgui/imgui_impl_dx9.cpp
    vendor/imgui/imgui_impl_win32.cpp
)

target_include_directories(internal PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/vendor
    ${CMAKE_SOURCE_DIR}/vendor/imgui
    ${DXSDK}/Include
)

target_link_directories(internal PRIVATE ${DXSDK}/Lib/x86)

target_link_libraries(internal PRIVATE
    user32 gdi32 kernel32 d3d9 dinput8 dxguid
)

set_target_properties(internal PROPERTIES
    OUTPUT_NAME "internal"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# ── injector.exe ──────────────────────────────────────────────────────────────
add_executable(injector injector/injector.cpp)

target_compile_options(injector PRIVATE /EHsc)

target_link_libraries(injector PRIVATE kernel32 user32)

set_target_properties(injector PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}
)
