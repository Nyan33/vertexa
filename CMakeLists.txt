cmake_minimum_required(VERSION 3.21)

project(Vertexa
    VERSION 0.1.0
    DESCRIPTION "Open-source 2D vector animation studio"
    HOMEPAGE_URL "https://github.com/Nyan33/vertexa"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(VERTEXA_BUILD_APP   "Build the Vertexa desktop application" ON)
option(VERTEXA_BUILD_TESTS "Build the unit tests" ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /wd4251 /wd4127)
    add_compile_definitions(_USE_MATH_DEFINES NOMINMAX)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# The geometry kernel is pure C++ and has no Qt dependency.
add_subdirectory(src/geom)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui OpenGL)
if(VERTEXA_BUILD_APP)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)
endif()

# The stage draws through Qt RHI (Vulkan, Metal, Direct3D, OpenGL) when Qt
# has it: 6.7 or later (QRhiWidget) with Qt Shader Tools. Otherwise the
# OpenGL / CPU paths are used.
option(VERTEXA_ENABLE_RHI "Draw the stage through Qt RHI when Qt supports it" ON)
set(VERTEXA_RHI OFF)
if(VERTEXA_ENABLE_RHI AND Qt6_VERSION VERSION_GREATER_EQUAL 6.7)
    find_package(Qt6 QUIET COMPONENTS ShaderTools)
    if(NOT TARGET Qt6::GuiPrivate)
        find_package(Qt6 QUIET COMPONENTS GuiPrivate)
    endif()
    if(Qt6ShaderTools_FOUND AND TARGET Qt6::GuiPrivate)
        set(VERTEXA_RHI ON)
    endif()
endif()
message(STATUS "Vertexa: RHI stage rendering: ${VERTEXA_RHI}")
set(CMAKE_AUTOMOC ON)

add_subdirectory(src/core)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/render/CMakeLists.txt)
    add_subdirectory(src/render)
endif()

if(VERTEXA_BUILD_APP AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/app/CMakeLists.txt)
    add_subdirectory(src/app)
endif()

if(VERTEXA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
