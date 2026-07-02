add_library(protoscope_plot STATIC
    plot/csv_data_file.cc
    plot/oscilloscope.cc
    plot/raw_capture_file.cc
    plot/wave_fft.cc
    plot/wave_math.cc
)
target_include_directories(protoscope_plot
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(protoscope_plot
    PUBLIC
        protoscope_pocketfft
)
target_compile_features(protoscope_plot PUBLIC cxx_std_20)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(protoscope_plot PRIVATE -Wno-maybe-uninitialized)
endif()

add_library(protoscope_session STATIC
    session/session_package.cc
)
target_include_directories(protoscope_session
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)
target_compile_features(protoscope_session PUBLIC cxx_std_20)

add_library(protoscope_protocol_utils STATIC
    protocol_utils/codec.cc
)
target_include_directories(protoscope_protocol_utils
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)
target_compile_features(protoscope_protocol_utils PUBLIC cxx_std_20)

add_library(protoscope_transport STATIC
    transport/serial_ports.cc
    transport/transport.cc
    transport/udp_transport.cc
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
    ui/dock_layout.cc
    ui/protocol_state_file.cc
    ui/protocol_ui_state.cc
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
    scripting/frame_stream_parser.cc
    scripting/pipeline_threading.cc
    scripting/script_runtime_worker.cc
    scripting/host/script_callback_dispatcher.cc
    scripting/host/script_file_system_facade.cc
    scripting/host/script_host_core.cc
    scripting/host/script_host_loader.cc
    scripting/host/stream_schema_module.cc
    scripting/host/core_api_module.cc
    scripting/host/tx_api_module.cc
    scripting/host/status_api_module.cc
    scripting/host/ui_api_module.cc
    scripting/host/file_api_module.cc
    scripting/host/oscilloscope_api_module.cc
    scripting/host/plot_api_module.cc
    scripting/host/control_api_module.cc
    scripting/host/codec_api_module.cc
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
        Threads::Threads
)
if(MINGW)
    target_link_libraries(protoscope_scripting PUBLIC winpthread)
endif()
target_compile_features(protoscope_scripting PUBLIC cxx_std_20)
target_compile_definitions(protoscope_scripting PRIVATE SOL_ALL_SAFETIES_ON=1)

add_library(protoscope_elf_static_view STATIC
    plugin/elf_static_view_bridge.cc
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
    dock/docks.cc
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
    config/config.cc
    config/embedded_protocols.cc
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
    logging/logging.cc
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

add_library(protoscope_startup_diagnostics STATIC
    app/startup_diagnostics.cc
    app/windows_args.cc
)
target_include_directories(protoscope_startup_diagnostics
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        ${PROJECT_BINARY_DIR}/generated/include
)
target_link_libraries(protoscope_startup_diagnostics
    PUBLIC
        protoscope_config
)
target_compile_features(protoscope_startup_diagnostics PUBLIC cxx_std_20)

add_library(protoscope_app STATIC
    app/application.cc
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
        protoscope_session
        protoscope_scripting
        protoscope_elf_static_view
        protoscope_transport
        protoscope_protocol_utils
        yaml-cpp::yaml-cpp
)
target_compile_features(protoscope_app PUBLIC cxx_std_20)

add_library(protoscope_update_check STATIC
    ui/update_check.cc
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
        ui/algorithm_help.cc
        ui/editable_combo.cc
        ui/keyboard_shortcuts.cc
        ui/ui_theme.cc
        ui/runtime/gui_runtime_core.cc
        ui/runtime/shortcut_controller.cc
        ui/runtime/ui_components.cc
        ui/runtime/platform_controller.cc
        ui/runtime/config_controller.cc
        ui/runtime/workspace_controller.cc
        ui/runtime/main_menu.cc
        ui/runtime/lua_view_menu.cc
        ui/runtime/help_menu.cc
        ui/docks/dock_host.cc
        ui/docks/offline_replay_dock.cc
        ui/docks/lua_dock_host.cc
        ui/dialogs/dialog_host.cc
        ui/wave/wave_channel_legend.cc
        ui/wave/wave_channel_legend_bar.cc
        ui/wave/wave_channel_renderer.cc
        ui/wave/wave_dock_renderer.cc
        ui/wave/wave_fft_layer.cc
        ui/wave/wave_interaction_controller.cc
        ui/wave/wave_measurement_overlay.cc
        ui/wave/wave_measurement_overlay_layer.cc
        ui/wave/wave_overview_layer.cc
        ui/wave/wave_phosphor_renderer.cc
        ui/wave/wave_plot_layer.cc
        ui/wave/wave_toolbar.cc
        ui/wave/wave_viewport_controller.cc
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
        PRIVATE
            ui_resources
    )
    target_compile_features(protoscope_ui PUBLIC cxx_std_20)
    if(WIN32)
        target_link_libraries(protoscope_ui PUBLIC shell32 ole32 comdlg32)
    endif()
endif()

if(PROTOSCOPE_ENABLE_GUI)
    if(WIN32)
        set(PROTOSCOPE_APP_ICON_RC_PATH "${PROJECT_SOURCE_DIR}/assets/icons/protoscope.ico")
        file(TO_CMAKE_PATH "${PROTOSCOPE_APP_ICON_RC_PATH}" PROTOSCOPE_APP_ICON_RC_PATH)
        set(PROTOSCOPE_WINDOWS_RESOURCE_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/ui/runtime/protoscope_windows_resource.h")
        file(TO_CMAKE_PATH "${PROTOSCOPE_WINDOWS_RESOURCE_HEADER}" PROTOSCOPE_WINDOWS_RESOURCE_HEADER)
        set(PROTOSCOPE_WINDOWS_RESOURCE_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated/protoscope.rc")
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/protoscope.rc.in" "${PROTOSCOPE_WINDOWS_RESOURCE_FILE}" @ONLY)

        add_executable(ProtoScope WIN32 main.cc "${PROTOSCOPE_WINDOWS_RESOURCE_FILE}")
    else()
        add_executable(ProtoScope main.cc)
    endif()

    target_link_libraries(ProtoScope
        PRIVATE
            protoscope_app
            protoscope_config
            protoscope_startup_diagnostics
            protoscope_ui
    )

    target_compile_features(ProtoScope PRIVATE cxx_std_20)
    if(WIN32)
        target_link_libraries(ProtoScope PRIVATE ws2_32)
        if(MSVC)
            target_link_options(ProtoScope PRIVATE "/DELAYLOAD:D3DCOMPILER_47.dll")
            target_link_libraries(ProtoScope PRIVATE delayimp)
        endif()
    endif()
endif()

foreach(target_name IN ITEMS
    protoscope_protocol_utils
    protoscope_transport
    protoscope_ui_layout
    protoscope_scripting
    protoscope_elf_static_view
    protoscope_dock
    protoscope_plot
    protoscope_session
    protoscope_config
    protoscope_startup_diagnostics
    protoscope_app
    protoscope_update_check
)
    protoscope_enable_warnings(${target_name})
endforeach()

if(PROTOSCOPE_ENABLE_GUI)
    foreach(target_name IN ITEMS protoscope_imgui protoscope_ui ProtoScope)
        protoscope_enable_warnings(${target_name})
    endforeach()
endif()
