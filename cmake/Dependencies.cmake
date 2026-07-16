include_guard(GLOBAL)

include(FetchContent)

add_subdirectory(3rdparty/spdlog)
add_subdirectory(3rdparty/yaml-cpp)

if(NOT TARGET libdwarf::dwarf-static)
    add_subdirectory(
        ${PROJECT_SOURCE_DIR}/3rdparty/ElfStaticView/3rdparty/libdwarf-code
        ${CMAKE_BINARY_DIR}/elf_static_view_libdwarf
        EXCLUDE_FROM_ALL
    )
endif()

# cmrc 使用本地 3rdparty 副本，避免配置阶段额外拉取。
include(${PROJECT_SOURCE_DIR}/3rdparty/cmrc/CMakeRC.cmake)

add_library(elf_static_view_core
    ${PROJECT_SOURCE_DIR}/3rdparty/ElfStaticView/src/analysis/address_bias.cc
    ${PROJECT_SOURCE_DIR}/3rdparty/ElfStaticView/src/analysis/export_document.cc
    ${PROJECT_SOURCE_DIR}/3rdparty/ElfStaticView/src/analysis/expander.cc
    ${PROJECT_SOURCE_DIR}/3rdparty/ElfStaticView/src/analysis/model_json.cc
    ${PROJECT_SOURCE_DIR}/3rdparty/ElfStaticView/src/analysis/model_utils.cc
    ${PROJECT_SOURCE_DIR}/3rdparty/ElfStaticView/src/analysis/project.cc
    ${PROJECT_SOURCE_DIR}/3rdparty/ElfStaticView/src/analysis/project_summary.cc
    ${PROJECT_SOURCE_DIR}/3rdparty/ElfStaticView/src/analysis/static_address_query.cc
    ${PROJECT_SOURCE_DIR}/3rdparty/ElfStaticView/src/elf/elf_symbol_table.cc
    ${PROJECT_SOURCE_DIR}/3rdparty/ElfStaticView/src/elf/dwarf_expression.cc
    ${PROJECT_SOURCE_DIR}/3rdparty/ElfStaticView/src/elf/dwarf_reader.cc
    ${PROJECT_SOURCE_DIR}/3rdparty/ElfStaticView/src/elf/raw_dwarf_reader.cc
    ${PROJECT_SOURCE_DIR}/3rdparty/ElfStaticView/src/elf/dwarf_wrappers.cc
    ${PROJECT_SOURCE_DIR}/3rdparty/ElfStaticView/src/elf/ti_coff_object.cc
    ${PROJECT_SOURCE_DIR}/3rdparty/ElfStaticView/src/logging/logger.cc
)
target_include_directories(elf_static_view_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/3rdparty/ElfStaticView/include
    PRIVATE
        ${PROJECT_SOURCE_DIR}/3rdparty/ElfStaticView/src
        ${PROJECT_SOURCE_DIR}/3rdparty/yaml-cpp/include
)
target_link_libraries(elf_static_view_core
    PUBLIC
        libdwarf::dwarf-static
    PRIVATE
        yaml-cpp::yaml-cpp
)
target_compile_features(elf_static_view_core PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(elf_static_view_core PRIVATE /utf-8)
endif()
if(WIN32)
    target_compile_definitions(elf_static_view_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
add_library(elf_static_view::core ALIAS elf_static_view_core)

add_library(protoscope_lua STATIC
    ${PROJECT_SOURCE_DIR}/3rdparty/lua/onelua.c
)
target_include_directories(protoscope_lua
    PUBLIC
        ${PROJECT_SOURCE_DIR}/3rdparty/lua
)
set_target_properties(protoscope_lua PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
target_compile_definitions(protoscope_lua PRIVATE MAKE_LIB)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(protoscope_lua PRIVATE -Wno-stringop-overflow)
endif()
if(UNIX)
    target_link_libraries(protoscope_lua PUBLIC m)
endif()

add_library(protoscope_sol2 INTERFACE)
target_include_directories(protoscope_sol2
    INTERFACE
        ${PROJECT_SOURCE_DIR}/3rdparty/sol2/include
)

add_library(protoscope_asio INTERFACE)
target_include_directories(protoscope_asio
    INTERFACE
        ${PROJECT_SOURCE_DIR}/3rdparty/asio/asio/include
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

add_library(protoscope_pocketfft INTERFACE)
target_include_directories(protoscope_pocketfft
    SYSTEM INTERFACE
        ${PROJECT_SOURCE_DIR}/3rdparty/pocketfft
)

set(PROTOSCOPE_IMGUI_ROOT ${PROJECT_SOURCE_DIR}/3rdparty/imgui)
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
    if(WIN32)
        target_sources(protoscope_imgui
            PRIVATE
                ${PROTOSCOPE_IMGUI_ROOT}/backends/imgui_impl_dx11.cpp
        )
    endif()
    target_include_directories(protoscope_imgui
        PUBLIC
            ${PROJECT_SOURCE_DIR}/include
            ${PROTOSCOPE_IMGUI_ROOT}
            ${PROTOSCOPE_IMGUI_ROOT}/backends
            ${PROTOSCOPE_IMGUI_ROOT}/examples/libs/glfw/include
    )
    target_compile_features(protoscope_imgui PUBLIC cxx_std_20)
    if(WIN32)
        target_compile_definitions(protoscope_imgui PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(protoscope_imgui PUBLIC opengl32 imm32 gdi32 d3d11 dxgi d3dcompiler)
        if(MSVC)
            target_link_directories(protoscope_imgui PUBLIC ${PROTOSCOPE_IMGUI_ROOT}/examples/libs/glfw/lib-vc2010-64)
            target_link_libraries(protoscope_imgui PUBLIC glfw3 legacy_stdio_definitions)
        else()
            target_link_libraries(protoscope_imgui PUBLIC glfw)
        endif()
    endif()
endif()

set(PROTOSCOPE_IMPLOT_ROOT ${PROJECT_SOURCE_DIR}/3rdparty/implot)
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
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(protoscope_implot PRIVATE -Wno-deprecated-enum-enum-conversion)
    endif()
endif()

message(STATUS "ProtoScope third-party roots:")
message(STATUS "  spdlog      -> ${PROJECT_SOURCE_DIR}/3rdparty/spdlog")
message(STATUS "  yaml-cpp    -> ${PROJECT_SOURCE_DIR}/3rdparty/yaml-cpp")
message(STATUS "  imgui       -> ${PROJECT_SOURCE_DIR}/3rdparty/imgui")
message(STATUS "  asio        -> ${PROJECT_SOURCE_DIR}/3rdparty/asio")
message(STATUS "  lua         -> ${PROJECT_SOURCE_DIR}/3rdparty/lua")
message(STATUS "  sol2        -> ${PROJECT_SOURCE_DIR}/3rdparty/sol2")
message(STATUS "  pocketfft   -> ${PROJECT_SOURCE_DIR}/3rdparty/pocketfft")
message(STATUS "  implot      -> ${PROTOSCOPE_IMPLOT_ROOT}")
