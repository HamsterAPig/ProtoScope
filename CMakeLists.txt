cmake_minimum_required(VERSION 3.21)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(ProtoScope VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)
find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(LIBDWARF_CRT "MT" CACHE STRING "Use the static MSVC runtime for libdwarf" FORCE)
endif()

option(PROTOSCOPE_BUILD_EXAMPLES "Build ProtoScope examples" ON)
option(SPDLOG_BUILD_EXAMPLE "Build spdlog examples" OFF)
option(SPDLOG_BUILD_TESTS "Build spdlog tests" OFF)
option(SPDLOG_BUILD_BENCH "Build spdlog bench" OFF)
option(YAML_CPP_BUILD_TESTS "Build yaml-cpp tests" OFF)
option(YAML_CPP_BUILD_TOOLS "Build yaml-cpp tools" OFF)
option(PROTOSCOPE_ENABLE_GUI "Build ProtoScope GUI runtime" ON)

include(cmake/CompilerWarnings.cmake)
include(cmake/Dependencies.cmake)
include(cmake/EmbeddedProtocols.cmake)
include(cmake/EmbeddedUiResources.cmake)
include(cmake/VersionInfo.cmake)
include(cmake/Format.cmake)

add_subdirectory(src)
if(BUILD_TESTING)
    if(PROTOSCOPE_ENABLE_GUI)
        add_subdirectory(tests)
    else()
        message(STATUS "Skipping tests because PROTOSCOPE_ENABLE_GUI=OFF disables the GUI-linked test target")
    endif()
endif()
