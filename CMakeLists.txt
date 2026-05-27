cmake_minimum_required(VERSION 3.21)

project(ProtoScope VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)
include(FetchContent)

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

add_subdirectory(3rdparty/spdlog)
add_subdirectory(3rdparty/yaml-cpp)

add_library(protoscope_lua STATIC
    3rdparty/lua/onelua.c
)
target_include_directories(protoscope_lua
    PUBLIC
        ${CMAKE_SOURCE_DIR}/3rdparty/lua
)
set_target_properties(protoscope_lua PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
target_compile_definitions(protoscope_lua PRIVATE MAKE_LIB)
if(UNIX)
    target_link_libraries(protoscope_lua PUBLIC m)
endif()

add_library(protoscope_sol2 INTERFACE)
target_include_directories(protoscope_sol2
    INTERFACE
        ${CMAKE_SOURCE_DIR}/3rdparty/sol2/include
)

add_library(protoscope_asio INTERFACE)
target_include_directories(protoscope_asio
    INTERFACE
        ${CMAKE_SOURCE_DIR}/3rdparty/asio/asio/include
)
target_compile_definitions(protoscope_asio
    INTERFACE
        ASIO_STANDALONE
)
if(WIN32)
    target_link_libraries(protoscope_asio
        INTERFACE
            ws2_32
            mswsock
    )
    target_compile_definitions(protoscope_asio
        INTERFACE
            _WIN32_WINNT=0x0601
    )
endif()

set(PROTOSCOPE_IMGUI_ROOT ${CMAKE_SOURCE_DIR}/3rdparty/imgui)
set(PROTOSCOPE_IMGUI_SOURCES
    ${PROTOSCOPE_IMGUI_ROOT}/imgui.cpp
    ${PROTOSCOPE_IMGUI_ROOT}/imgui_draw.cpp
    ${PROTOSCOPE_IMGUI_ROOT}/imgui_tables.cpp
    ${PROTOSCOPE_IMGUI_ROOT}/imgui_widgets.cpp
    ${PROTOSCOPE_IMGUI_ROOT}/imgui_demo.cpp
)

if(PROTOSCOPE_ENABLE_GUI)
    if(WIN32 AND NOT MSVC)
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            glfw
            GIT_REPOSITORY https://github.com/glfw/glfw.git
            GIT_TAG 3.4
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(glfw)
    endif()

    add_library(protoscope_imgui STATIC
        ${PROTOSCOPE_IMGUI_SOURCES}
        ${PROTOSCOPE_IMGUI_ROOT}/backends/imgui_impl_glfw.cpp
        ${PROTOSCOPE_IMGUI_ROOT}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(protoscope_imgui
        PUBLIC
            ${CMAKE_SOURCE_DIR}/include
            ${PROTOSCOPE_IMGUI_ROOT}
            ${PROTOSCOPE_IMGUI_ROOT}/backends
            ${PROTOSCOPE_IMGUI_ROOT}/examples/libs/glfw/include
    )
    target_compile_features(protoscope_imgui PUBLIC cxx_std_20)
    if(WIN32)
        target_compile_definitions(protoscope_imgui PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(protoscope_imgui PUBLIC opengl32 imm32 gdi32)
        if(MSVC)
            target_link_directories(protoscope_imgui PUBLIC ${PROTOSCOPE_IMGUI_ROOT}/examples/libs/glfw/lib-vc2010-64)
            target_link_libraries(protoscope_imgui PUBLIC glfw3 legacy_stdio_definitions)
        else()
            target_link_libraries(protoscope_imgui PUBLIC glfw)
        endif()
    endif()
endif()

set(PROTOSCOPE_IMPLOT_ROOT ${CMAKE_SOURCE_DIR}/3rdparty/implot)
if(PROTOSCOPE_ENABLE_GUI)
    if(NOT EXISTS ${PROTOSCOPE_IMPLOT_ROOT}/implot.h)
        FetchContent_Declare(
            implot
            GIT_REPOSITORY https://github.com/epezent/implot.git
            GIT_TAG v1.0
            GIT_SHALLOW TRUE
            SOURCE_DIR ${PROTOSCOPE_IMPLOT_ROOT}
        )
        FetchContent_MakeAvailable(implot)
    endif()

    add_library(protoscope_implot STATIC
        ${PROTOSCOPE_IMPLOT_ROOT}/implot.cpp
        ${PROTOSCOPE_IMPLOT_ROOT}/implot_items.cpp
    )
    target_include_directories(protoscope_implot
        PUBLIC
            ${PROTOSCOPE_IMPLOT_ROOT}
            ${PROTOSCOPE_IMGUI_ROOT}
    )
    target_link_libraries(protoscope_implot
        PUBLIC
            protoscope_imgui
    )
    target_compile_features(protoscope_implot PUBLIC cxx_std_20)
endif()

add_library(protoscope_plot STATIC
    src/plot/oscilloscope.cpp
    src/plot/wave_math.cpp
)
target_include_directories(protoscope_plot
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)
target_compile_features(protoscope_plot PUBLIC cxx_std_20)

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
        protoscope_asio
)
target_compile_features(protoscope_transport PUBLIC cxx_std_20)

add_library(protoscope_ui_layout STATIC
    src/ui/dock_layout.cpp
)
target_include_directories(protoscope_ui_layout
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(protoscope_ui_layout
    PUBLIC
        protoscope_protocol_utils
        protoscope_plot
        protoscope_transport
        yaml-cpp::yaml-cpp
)
target_compile_features(protoscope_ui_layout PUBLIC cxx_std_20)

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
        protoscope_lua
        protoscope_sol2
)
target_compile_features(protoscope_scripting PUBLIC cxx_std_20)
target_compile_definitions(protoscope_scripting PRIVATE SOL_ALL_SAFETIES_ON=1)

add_library(protoscope_dock STATIC
    src/dock/docks.cpp
)
target_include_directories(protoscope_dock
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(protoscope_dock
    PUBLIC
        protoscope_plot
        protoscope_scripting
        protoscope_transport
)
target_compile_features(protoscope_dock PUBLIC cxx_std_20)

add_library(protoscope_config STATIC
    src/config/config.cpp
)
target_include_directories(protoscope_config
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(protoscope_config
    PUBLIC
        protoscope_dock
        yaml-cpp::yaml-cpp
)
target_compile_features(protoscope_config PUBLIC cxx_std_20)

add_library(protoscope_logging STATIC
    src/logging/logging.cpp
)
target_include_directories(protoscope_logging
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(protoscope_logging
    PUBLIC
        protoscope_config
        protoscope_dock
        spdlog::spdlog
)
target_compile_features(protoscope_logging PUBLIC cxx_std_20)

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
        protoscope_config
        protoscope_logging
        protoscope_scripting
        protoscope_transport
        protoscope_protocol_utils
        yaml-cpp::yaml-cpp
)
target_compile_features(protoscope_app PUBLIC cxx_std_20)

if(PROTOSCOPE_ENABLE_GUI)
    add_library(protoscope_ui STATIC
        src/ui/editable_combo.cpp
        src/ui/gui_runtime.cpp
        src/ui/wave_dock_renderer.cpp
    )
    target_include_directories(protoscope_ui
        PUBLIC
            ${CMAKE_SOURCE_DIR}/include
    )
    target_link_libraries(protoscope_ui
        PUBLIC
            protoscope_app
            protoscope_config
            protoscope_ui_layout
            protoscope_imgui
            protoscope_implot
            protoscope_plot
    )
    target_compile_features(protoscope_ui PUBLIC cxx_std_20)
    if(WIN32)
        target_link_libraries(protoscope_ui PUBLIC shell32 ole32)
    endif()
endif()

if(WIN32)
    add_executable(ProtoScope WIN32 src/main.cpp)
else()
    add_executable(ProtoScope src/main.cpp)
endif()

target_link_libraries(ProtoScope
    PRIVATE
        protoscope_app
        protoscope_config
        $<$<BOOL:${PROTOSCOPE_ENABLE_GUI}>:protoscope_ui>
)

target_compile_features(ProtoScope PRIVATE cxx_std_20)
if(WIN32)
    target_link_libraries(ProtoScope PRIVATE ws2_32)
endif()

foreach(target_name IN ITEMS
    protoscope_protocol_utils
    protoscope_transport
    protoscope_ui_layout
    protoscope_scripting
    protoscope_dock
    protoscope_plot
    protoscope_config
    protoscope_app
    ProtoScope
)
if(MSVC)
    target_compile_options(${target_name} PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
endif()
endforeach()

if(PROTOSCOPE_ENABLE_GUI)
    foreach(target_name IN ITEMS protoscope_imgui protoscope_ui)
        if(MSVC)
            target_compile_options(${target_name} PRIVATE /W4 /permissive- /utf-8)
        else()
            target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    endforeach()
endif()

add_executable(protoscope_tests
    tests/test_main.cpp
    tests/test_application.cpp
    tests/test_codec.cpp
    tests/test_dock.cpp
    tests/test_dock_layout.cpp
    tests/test_plot.cpp
    tests/test_script_host.cpp
    tests/test_transport.cpp
)
target_include_directories(protoscope_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(protoscope_tests
    PRIVATE
        protoscope_config
        protoscope_dock
        protoscope_ui_layout
        protoscope_app
        protoscope_protocol_utils
        protoscope_plot
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
set_tests_properties(protoscope_unit_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

message(STATUS "ProtoScope third-party roots:")
message(STATUS "  spdlog      -> ${CMAKE_SOURCE_DIR}/3rdparty/spdlog")
message(STATUS "  yaml-cpp    -> ${CMAKE_SOURCE_DIR}/3rdparty/yaml-cpp")
message(STATUS "  imgui       -> ${CMAKE_SOURCE_DIR}/3rdparty/imgui")
message(STATUS "  libdwarf    -> ${CMAKE_SOURCE_DIR}/3rdparty/libdwarf-code")
message(STATUS "  asio        -> ${CMAKE_SOURCE_DIR}/3rdparty/asio")
message(STATUS "  lua         -> ${CMAKE_SOURCE_DIR}/3rdparty/lua")
message(STATUS "  sol2        -> ${CMAKE_SOURCE_DIR}/3rdparty/sol2")
    message(STATUS "  implot      -> ${PROTOSCOPE_IMPLOT_ROOT}")
