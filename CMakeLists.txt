cmake_minimum_required(VERSION 3.21)

project(ProtoScope VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PROTOSCOPE_BUILD_EXAMPLES "Build ProtoScope examples" ON)
option(SPDLOG_BUILD_EXAMPLE "Build spdlog examples" OFF)
option(SPDLOG_BUILD_TESTS "Build spdlog tests" OFF)
option(SPDLOG_BUILD_BENCH "Build spdlog bench" OFF)
option(YAML_CPP_BUILD_TESTS "Build yaml-cpp tests" OFF)
option(YAML_CPP_BUILD_TOOLS "Build yaml-cpp tools" OFF)

add_subdirectory(3rdparty/spdlog)
add_subdirectory(3rdparty/yaml-cpp)

add_executable(ProtoScope
    src/main.cpp
)

target_link_libraries(ProtoScope
    PRIVATE
        spdlog::spdlog
        yaml-cpp::yaml-cpp
)

target_compile_features(ProtoScope PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(ProtoScope PRIVATE /W4 /permissive-)
else()
    target_compile_options(ProtoScope PRIVATE -Wall -Wextra -Wpedantic)
endif()

message(STATUS "ProtoScope third-party roots:")
message(STATUS "  spdlog      -> ${CMAKE_SOURCE_DIR}/3rdparty/spdlog")
message(STATUS "  yaml-cpp    -> ${CMAKE_SOURCE_DIR}/3rdparty/yaml-cpp")
message(STATUS "  imgui       -> ${CMAKE_SOURCE_DIR}/3rdparty/imgui")
message(STATUS "  libdwarf    -> ${CMAKE_SOURCE_DIR}/3rdparty/libdwarf-code")

