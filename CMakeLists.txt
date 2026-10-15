cmake_minimum_required(VERSION 3.21)

project(Vertexa VERSION 2.0.0 DESCRIPTION "2D vector animation studio" LANGUAGES C RC)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

if(NOT WIN32)
    message(FATAL_ERROR "Vertexa v2 currently targets Windows (D3D11). Other platforms come later.")
endif()

set(VX_SOURCES
    src/main.c
    src/impl.c
    src/assets.c
    src/gfx/render.c
    src/gfx/font.c
    src/gfx/logo.c
    src/gfx/cursors.c
    src/ui/ui.c
    src/doc/doc.c
    src/doc/io.c
    src/doc/bitmap.c
    src/geom/geom.c
    src/render/scene.c
    src/brush/brush.c
    src/app/app.c
    src/app/stage.c
    src/app/timeline.c
    src/app/panels.c
    src/app/trim.c
    src/app/select.c
    src/app/shapes.c
    src/app/export.c
    src/app/settings.c
    src/app/keys.c
    src/app/home.c
    src/app/exporter.c
    src/io/fla.c
    src/io/xml.c
    src/io/sut.c
    src/audio/audio.c
    src/audio/decoders.c
    src/platform/win32.c
    src/platform/capture_win32.c
)

# the exe icon Explorer shows (the window icon is set at run time from the embedded PNG)
file(WRITE ${CMAKE_BINARY_DIR}/vertexa.rc "1 ICON \"${CMAKE_SOURCE_DIR}/assets/brand/vertexa.ico\"\n")
list(APPEND VX_SOURCES ${CMAKE_BINARY_DIR}/vertexa.rc)

add_executable(vertexa ${VX_SOURCES})
set_source_files_properties(src/audio/decoders.c PROPERTIES COMPILE_OPTIONS "-w")   # third-party decoders
target_include_directories(vertexa PRIVATE src third_party)
target_compile_definitions(vertexa PRIVATE
    SOKOL_D3D11
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    VX_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets"
)
target_compile_options(vertexa PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function -Wno-sign-compare -Wno-type-limits)
target_link_libraries(vertexa PRIVATE kernel32 user32 shell32 gdi32 ole32 dxgi d3d11 dxguid dwmapi comdlg32 advapi32 m)

# fonts and icons are embedded into the executable (see src/assets.c)
file(GLOB VX_FONT_FILES ${CMAKE_SOURCE_DIR}/assets/fonts/*.ttf ${CMAKE_SOURCE_DIR}/assets/icons/*.ttf ${CMAKE_SOURCE_DIR}/assets/brand/*.png)
set_source_files_properties(src/assets.c PROPERTIES OBJECT_DEPENDS "${VX_FONT_FILES}")

# Debug and RelWithDebInfo keep a console for logs; Release is a pure GUI app.
target_link_options(vertexa PRIVATE $<IF:$<CONFIG:Release>,-mwindows,-mconsole> -static)   # no MinGW DLLs next to the exe
