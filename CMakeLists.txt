cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# Volchay-fastcut: a fast-startup, native Windows video editor.
# ---------------------------------------------------------------------------
project(volchay_fastcut
        VERSION      0.1.0
        DESCRIPTION  "Fast-startup native Windows 11 video editor"
        LANGUAGES    CXX C)

if(NOT WIN32)
    message(FATAL_ERROR
        "Volchay-fastcut targets Windows only. "
        "Use the MinGW-w64 toolchain file from Linux: "
        "-DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-toolchain.cmake")
endif()

set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)
set(CMAKE_C_STANDARD            11)
set(CMAKE_C_STANDARD_REQUIRED   ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# On MSVC, link the static C runtime so the produced .exe has no
# vcruntime / msvcp DLL dependency, mirroring what the MinGW toolchain
# achieves with -static-libstdc++.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Aggressive size + startup optimisations in Release. The flag set differs
# between MSVC and the GCC-compatible toolchains (MinGW, Clang) — using the
# wrong syntax breaks the other compiler.
if(MSVC)
    if(CMAKE_BUILD_TYPE STREQUAL "Release"
       OR "Release" IN_LIST CMAKE_CONFIGURATION_TYPES)
        add_compile_options($<$<CONFIG:Release>:/O2>
                            $<$<CONFIG:Release>:/Gy>
                            $<$<CONFIG:Release>:/Gw>
                            $<$<CONFIG:Release>:/GR->)
        add_link_options($<$<CONFIG:Release>:/OPT:REF>
                         $<$<CONFIG:Release>:/OPT:ICF>)
    endif()
else()
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O2 -ffunction-sections -fdata-sections -fno-rtti)
        add_link_options(-Wl,--gc-sections -Wl,-s -Wl,--as-needed)
    endif()
endif()

add_compile_definitions(
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00          # Windows 10 / 11
    WINVER=0x0A00
    VOLCHAY_VERSION_STRING="${PROJECT_VERSION}"
)

# Warnings.
if(MSVC)
    add_compile_options(/W3 /wd4100 /wd4201 /wd4324)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(
        -Wall -Wextra
        -Wno-unused-parameter -Wno-missing-field-initializers
    )
endif()

# ---------------------------------------------------------------------------
# FFmpeg / libav (vendored prebuilt, BtbN MinGW build).
# Headers under third_party/ffmpeg/include, MinGW import libs in
# third_party/ffmpeg/lib, and the .dll runtime in third_party/ffmpeg/bin
# (copied next to volchay-fastcut.exe at build time).
# ---------------------------------------------------------------------------
set(FFMPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg")
add_library(ffmpeg INTERFACE)
target_include_directories(ffmpeg INTERFACE "${FFMPEG_DIR}/include")
target_link_libraries(ffmpeg INTERFACE
    "${FFMPEG_DIR}/lib/libavformat.dll.a"
    "${FFMPEG_DIR}/lib/libavcodec.dll.a"
    "${FFMPEG_DIR}/lib/libavutil.dll.a"
    "${FFMPEG_DIR}/lib/libswscale.dll.a"
    "${FFMPEG_DIR}/lib/libswresample.dll.a"
)

set(FFMPEG_RUNTIME_DLLS
    "${FFMPEG_DIR}/bin/avformat-62.dll"
    "${FFMPEG_DIR}/bin/avcodec-62.dll"
    "${FFMPEG_DIR}/bin/avutil-60.dll"
    "${FFMPEG_DIR}/bin/swscale-9.dll"
    "${FFMPEG_DIR}/bin/swresample-6.dll"
)

# ---------------------------------------------------------------------------
# Dear ImGui (docking branch, vendored).
# ---------------------------------------------------------------------------
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
add_library(imgui STATIC
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
)
target_include_directories(imgui PUBLIC
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
)
target_compile_definitions(imgui PUBLIC
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    IMGUI_DISABLE_DEMO_WINDOWS
    IMGUI_DISABLE_DEBUG_TOOLS
)

# ---------------------------------------------------------------------------
# Main executable.
# ---------------------------------------------------------------------------
set(VOLCHAY_SOURCES
    src/main.cpp

    src/app/app.cpp
    src/app/window.cpp

    src/render/d3d_context.cpp

    src/media/mf_player.cpp
    src/media/audio_player.cpp
    src/media/exporter.cpp

    src/core/project.cpp

    src/ui/theme.cpp
    src/ui/fonts.cpp
    src/ui/main_layout.cpp
    src/ui/panels/viewer.cpp
    src/ui/panels/timeline.cpp
    src/ui/panels/library.cpp
    src/ui/panels/inspector.cpp
    src/ui/panels/settings.cpp
    src/ui/panels/export_panel.cpp

    src/util/log.cpp
    src/util/settings.cpp

    src/shell/registry.cpp
)

set(VOLCHAY_RC src/app/app.rc)

add_executable(volchay-fastcut WIN32
    ${VOLCHAY_SOURCES}
    ${VOLCHAY_RC}
)

target_include_directories(volchay-fastcut PRIVATE src)

target_link_libraries(volchay-fastcut PRIVATE
    imgui
    ffmpeg
    # Core Win32
    user32 gdi32 ole32 oleaut32 uuid shell32 shlwapi advapi32 comctl32 comdlg32
    # Direct3D / DXGI
    d3d11 dxgi d3dcompiler
    # Direct2D / DirectWrite (used for some UI primitives)
    d2d1 dwrite
    # DirectComposition
    dcomp
    # DWM (used by Dear ImGui Win32 backend for alpha compositing)
    dwmapi
    # Media Foundation (still used by the H.264/AAC exporter)
    mfplat mfreadwrite mfuuid mf
    # WASAPI / MMDevice for audio playback
    winmm
    # Windows Imaging
    windowscodecs
)

# Copy the ffmpeg runtime DLLs next to the executable so they're
# resolved from the same directory at startup.
add_custom_command(TARGET volchay-fastcut POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${FFMPEG_RUNTIME_DLLS}
            "$<TARGET_FILE_DIR:volchay-fastcut>"
    COMMAND_EXPAND_LISTS
)

# Use the Unicode entry point (wWinMain) on MinGW, which requires -municode.
if(MINGW)
    target_link_options(volchay-fastcut PRIVATE -municode)
endif()
# We embed our own manifest via app.rc; tell the MSVC linker not to also
# emit one, otherwise CVTRES errors with "duplicate resource" / LNK1123.
if(MSVC)
    target_link_options(volchay-fastcut PRIVATE /MANIFEST:NO)
endif()

set_target_properties(volchay-fastcut PROPERTIES
    OUTPUT_NAME "volchay-fastcut"
)

# ---------------------------------------------------------------------------
# Standalone tool: register / unregister Windows context menu entries.
# Built as a tiny console app so an installer (or the user) can run it
# from a script without launching the full editor.
# ---------------------------------------------------------------------------
add_executable(volchay-register
    src/shell/register_main.cpp
    src/shell/registry.cpp
    src/util/log.cpp
)
target_include_directories(volchay-register PRIVATE src)
target_link_libraries(volchay-register PRIVATE
    user32 advapi32 shell32 shlwapi ole32
)
if(MINGW)
    # wmain entry point requires -municode just like wWinMain.
    target_link_options(volchay-register PRIVATE -municode)
endif()

# ---------------------------------------------------------------------------
# Install layout.
# ---------------------------------------------------------------------------
install(TARGETS volchay-fastcut volchay-register
        RUNTIME DESTINATION bin)
install(FILES ${FFMPEG_RUNTIME_DLLS}
        DESTINATION bin)
install(FILES installer/register-context-menu.bat
              installer/unregister-context-menu.bat
        DESTINATION bin)
install(FILES README.md LICENSE
        DESTINATION .)
