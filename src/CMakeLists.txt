add_library(protoscope_plot STATIC
    plot/oscilloscope.cpp
    plot/raw_capture_file.cpp
    plot/wave_math.cpp
)
target_include_directories(protoscope_plot
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)
target_compile_features(protoscope_plot PUBLIC cxx_std_20)

add_library(protoscope_protocol_utils STATIC
    protocol_utils/codec.cpp
)
target_include_directories(protoscope_protocol_utils
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)
target_compile_features(protoscope_protocol_utils PUBLIC cxx_std_20)

add_library(protoscope_transport STATIC
    transport/serial_ports.cpp
    transport/transport.cpp
    transport/udp_transport.cpp
)
target_include_directories(protoscope_transport
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(protoscope_transport
    PUBLIC
        protoscope_protocol_utils
        protoscope_asio
)
target_compile_features(protoscope_transport PUBLIC cxx_std_20)

add_library(protoscope_ui_layout STATIC
    ui/dock_layout.cpp
    ui/protocol_ui_state.cpp
)
target_include_directories(protoscope_ui_layout
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(protoscope_ui_layout
    PUBLIC
        protoscope_protocol_utils
        protoscope_plot
        protoscope_transport
        protoscope_sol2
        yaml-cpp::yaml-cpp
)
target_compile_features(protoscope_ui_layout PUBLIC cxx_std_20)

add_library(protoscope_scripting STATIC
    scripting/frame_stream_parser.cpp
    scripting/script_host.cpp
)
target_include_directories(protoscope_scripting
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
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

add_library(protoscope_elf_static_view STATIC
    plugin/elf_static_view_bridge.cpp
)
target_include_directories(protoscope_elf_static_view
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(protoscope_elf_static_view
    PUBLIC
        elf_static_view::core
)
target_compile_features(protoscope_elf_static_view PUBLIC cxx_std_20)

# Lua UI 布局会解析脚本声明，因此在脚本宿主目标创建后再补齐依赖。
target_link_libraries(protoscope_ui_layout
    PUBLIC
        protoscope_scripting
)

add_library(protoscope_dock STATIC
    dock/docks.cpp
)
target_include_directories(protoscope_dock
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(protoscope_dock
    PUBLIC
        protoscope_plot
        protoscope_protocol_utils
        protoscope_scripting
        protoscope_transport
)
target_compile_features(protoscope_dock PUBLIC cxx_std_20)

add_library(protoscope_config STATIC
    config/config.cpp
    config/embedded_protocols.cpp
)
target_include_directories(protoscope_config
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        ${PROJECT_BINARY_DIR}/generated
)
target_link_libraries(protoscope_config
    PUBLIC
        protoscope_dock
        yaml-cpp::yaml-cpp
    PRIVATE
        proto_resources
)
target_compile_features(protoscope_config PUBLIC cxx_std_20)

add_library(protoscope_logging STATIC
    logging/logging.cpp
)
target_include_directories(protoscope_logging
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(protoscope_logging
    PUBLIC
        protoscope_config
        protoscope_dock
        spdlog::spdlog
)
target_compile_features(protoscope_logging PUBLIC cxx_std_20)

add_library(protoscope_app STATIC
    app/application.cpp
)
target_include_directories(protoscope_app
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(protoscope_app
    PUBLIC
        protoscope_dock
        protoscope_config
        protoscope_logging
        protoscope_plot
        protoscope_scripting
        protoscope_elf_static_view
        protoscope_transport
        protoscope_protocol_utils
        yaml-cpp::yaml-cpp
)
target_compile_features(protoscope_app PUBLIC cxx_std_20)

add_library(protoscope_update_check STATIC
    ui/update_check.cpp
)
target_include_directories(protoscope_update_check
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        ${PROJECT_BINARY_DIR}/generated/include
)
target_compile_features(protoscope_update_check PUBLIC cxx_std_20)
if(WIN32)
    target_link_libraries(protoscope_update_check PUBLIC winhttp)
endif()

if(PROTOSCOPE_ENABLE_GUI)
    add_library(protoscope_ui STATIC
        ui/editable_combo.cpp
        ui/gui_runtime.cpp
        ui/wave_dock_renderer.cpp
    )
    target_include_directories(protoscope_ui
        PUBLIC
            ${PROJECT_SOURCE_DIR}/include
        PRIVATE
            ${PROJECT_BINARY_DIR}/generated/include
    )
    target_link_libraries(protoscope_ui
        PUBLIC
            protoscope_app
            protoscope_config
            protoscope_ui_layout
            protoscope_update_check
            protoscope_imgui
            protoscope_implot
            protoscope_plot
    )
    target_compile_features(protoscope_ui PUBLIC cxx_std_20)
    if(WIN32)
        target_link_libraries(protoscope_ui PUBLIC shell32 ole32 comdlg32)
    endif()
endif()

if(WIN32)
    add_executable(ProtoScope WIN32 main.cpp)
else()
    add_executable(ProtoScope main.cpp)
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
    protoscope_elf_static_view
    protoscope_dock
    protoscope_plot
    protoscope_config
    protoscope_app
    protoscope_update_check
    ProtoScope
)
    protoscope_enable_warnings(${target_name})
endforeach()

if(PROTOSCOPE_ENABLE_GUI)
    foreach(target_name IN ITEMS protoscope_imgui protoscope_ui)
        protoscope_enable_warnings(${target_name})
    endforeach()
endif()
