cmake_minimum_required(VERSION 3.21)

project(ProtoScope VERSION 0.1.0 LANGUAGES CXX)

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

add_subdirectory(3rdparty/spdlog)
add_subdirectory(3rdparty/yaml-cpp)

add_library(protoscope_protocol_utils STATIC
    src/protocol_utils/codec.cpp
)
target_include_directories(protoscope_protocol_utils
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)
target_compile_features(protoscope_protocol_utils PUBLIC cxx_std_20)

add_library(protoscope_transport STATIC
    src/transport/transport.cpp
)
target_include_directories(protoscope_transport
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(protoscope_transport
    PUBLIC
        protoscope_protocol_utils
)
target_compile_features(protoscope_transport PUBLIC cxx_std_20)

add_library(protoscope_scripting STATIC
    src/scripting/script_host.cpp
)
target_include_directories(protoscope_scripting
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(protoscope_scripting
    PUBLIC
        protoscope_protocol_utils
        protoscope_transport
)
target_compile_features(protoscope_scripting PUBLIC cxx_std_20)

add_library(protoscope_dock STATIC
    src/dock/docks.cpp
)
target_include_directories(protoscope_dock
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(protoscope_dock
    PUBLIC
        protoscope_scripting
        protoscope_transport
)
target_compile_features(protoscope_dock PUBLIC cxx_std_20)

add_library(protoscope_app STATIC
    src/app/application.cpp
)
target_include_directories(protoscope_app
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(protoscope_app
    PUBLIC
        protoscope_dock
        protoscope_scripting
        protoscope_transport
        protoscope_protocol_utils
        spdlog::spdlog
        yaml-cpp::yaml-cpp
)
target_compile_features(protoscope_app PUBLIC cxx_std_20)

add_executable(ProtoScope src/main.cpp)

target_link_libraries(ProtoScope
    PRIVATE
        protoscope_app
)

target_compile_features(ProtoScope PRIVATE cxx_std_20)

foreach(target_name IN ITEMS
    protoscope_protocol_utils
    protoscope_transport
    protoscope_scripting
    protoscope_dock
    protoscope_app
    ProtoScope
)
if(MSVC)
    target_compile_options(${target_name} PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
endif()
endforeach()

add_executable(protoscope_tests
    tests/test_main.cpp
    tests/test_codec.cpp
    tests/test_script_host.cpp
)
target_include_directories(protoscope_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(protoscope_tests
    PRIVATE
        protoscope_protocol_utils
        protoscope_scripting
        protoscope_transport
)
target_compile_features(protoscope_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(protoscope_tests PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(protoscope_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME protoscope_unit_tests COMMAND protoscope_tests)

message(STATUS "ProtoScope third-party roots:")
message(STATUS "  spdlog      -> ${CMAKE_SOURCE_DIR}/3rdparty/spdlog")
message(STATUS "  yaml-cpp    -> ${CMAKE_SOURCE_DIR}/3rdparty/yaml-cpp")
message(STATUS "  imgui       -> ${CMAKE_SOURCE_DIR}/3rdparty/imgui")
message(STATUS "  libdwarf    -> ${CMAKE_SOURCE_DIR}/3rdparty/libdwarf-code")
message(STATUS "  lua/sol2/asio/implot -> TODO: add submodules before real runtime integration")
