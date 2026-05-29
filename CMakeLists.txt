cmake_minimum_required(VERSION 3.21)

project(ProtoScope VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

add_subdirectory(src)
add_subdirectory(tests)
