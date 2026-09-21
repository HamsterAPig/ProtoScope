add_executable(protoscope_data_transfer_tests test_data_transfer.cc)
add_executable(protoscope_data_transfer_application_tests test_data_transfer_application.cc)
target_link_libraries(protoscope_data_transfer_application_tests PRIVATE protoscope_app)
add_test(NAME protoscope_data_transfer_application_tests COMMAND protoscope_data_transfer_application_tests)
set_tests_properties(protoscope_data_transfer_application_tests PROPERTIES TIMEOUT 60)
target_link_libraries(protoscope_data_transfer_tests PRIVATE protoscope_plot)
add_test(NAME protoscope_data_transfer_tests COMMAND protoscope_data_transfer_tests)
set_tests_properties(protoscope_data_transfer_tests PROPERTIES TIMEOUT 60)

add_executable(protoscope_wave_query_tests test_wave_query.cc)
target_link_libraries(protoscope_wave_query_tests PRIVATE protoscope_plot)
add_test(NAME protoscope_wave_query_tests COMMAND protoscope_wave_query_tests)
set_tests_properties(protoscope_wave_query_tests PROPERTIES TIMEOUT 60)

add_executable(protoscope_wave_pan_benchmark benchmark_wave_pan.cc)
target_link_libraries(protoscope_wave_pan_benchmark PRIVATE protoscope_plot)
add_executable(protoscope_wave_analysis_tests test_wave_analysis.cc)
target_link_libraries(protoscope_wave_analysis_tests PRIVATE protoscope_plot)
add_test(NAME protoscope_wave_analysis_tests COMMAND protoscope_wave_analysis_tests)
set_tests_properties(protoscope_wave_analysis_tests PROPERTIES TIMEOUT 60)

set(PROTOSCOPE_HEADLESS_TEST_SOURCES
    test_headless_main.cc
    test_headless_core.cc
    test_headless_plot.cc
    test_application.cc
    test_codec.cc
    test_frame_stream_parser.cc
    test_script_runtime_worker.cc
    test_serial_ports.cc
    test_transport.cc
)

add_executable(protoscope_headless_tests
    ${PROTOSCOPE_HEADLESS_TEST_SOURCES}
)
target_include_directories(protoscope_headless_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(protoscope_headless_tests
    PRIVATE
        protoscope_app
        protoscope_config
        protoscope_dock
        protoscope_elf_static_view
        protoscope_plot
        protoscope_protocol_utils
        protoscope_scripting
        protoscope_session
        protoscope_transport
)
target_compile_features(protoscope_headless_tests PRIVATE cxx_std_20)
protoscope_enable_warnings(protoscope_headless_tests)

add_test(NAME protoscope_headless_unit_tests COMMAND protoscope_headless_tests)
set_tests_properties(protoscope_headless_unit_tests PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    TIMEOUT 60
)

if(PROTOSCOPE_ENABLE_GUI)
    add_executable(protoscope_theme_render_tests test_theme_render.cc)
    target_link_libraries(protoscope_theme_render_tests PRIVATE protoscope_ui)
    add_executable(protoscope_theme_manager_tests test_theme_manager.cc)
    target_link_libraries(protoscope_theme_manager_tests PRIVATE protoscope_ui)
    add_test(NAME protoscope_theme_manager_tests COMMAND protoscope_theme_manager_tests)
    set_tests_properties(protoscope_theme_manager_tests PROPERTIES TIMEOUT 60)
    add_executable(protoscope_wave_downsample_mode_tests test_wave_downsample_mode.cc)
    target_link_libraries(protoscope_wave_downsample_mode_tests PRIVATE protoscope_ui protoscope_config)
    add_test(NAME protoscope_wave_downsample_mode_tests COMMAND protoscope_wave_downsample_mode_tests)
    set_tests_properties(protoscope_wave_downsample_mode_tests PROPERTIES TIMEOUT 60)
    add_executable(protoscope_wave_overview_color_tests test_wave_overview_color.cc)
    target_link_libraries(protoscope_wave_overview_color_tests PRIVATE protoscope_ui)
    add_test(NAME protoscope_wave_overview_color_tests COMMAND protoscope_wave_overview_color_tests)
    set_tests_properties(protoscope_wave_overview_color_tests PROPERTIES TIMEOUT 60)
    add_executable(protoscope_auxiliary_cursor_tests test_auxiliary_cursors.cc)
    target_link_libraries(protoscope_auxiliary_cursor_tests PRIVATE protoscope_ui)
    add_test(NAME protoscope_auxiliary_cursor_tests COMMAND protoscope_auxiliary_cursor_tests)
    set_tests_properties(protoscope_auxiliary_cursor_tests PROPERTIES TIMEOUT 60)
    add_executable(protoscope_wave_fft_live_tests test_wave_fft_live.cc)
    target_link_libraries(protoscope_wave_fft_live_tests PRIVATE protoscope_ui)
    add_test(NAME protoscope_wave_fft_live_tests COMMAND protoscope_wave_fft_live_tests)
    set_tests_properties(protoscope_wave_fft_live_tests PROPERTIES TIMEOUT 60)
    add_executable(protoscope_log_scroll_tests test_log_scroll.cc)
    target_include_directories(protoscope_log_scroll_tests PRIVATE ${PROJECT_BINARY_DIR}/generated/include)
    target_link_libraries(protoscope_log_scroll_tests PRIVATE protoscope_ui)
    add_test(NAME protoscope_log_scroll_tests COMMAND protoscope_log_scroll_tests)
    set_tests_properties(protoscope_log_scroll_tests PROPERTIES TIMEOUT 60)
    add_executable(protoscope_wave_fft_layout_tests test_wave_fft_layout.cc)
    target_link_libraries(protoscope_wave_fft_layout_tests PRIVATE protoscope_ui)
    add_test(NAME protoscope_wave_fft_layout_tests COMMAND protoscope_wave_fft_layout_tests)
    set_tests_properties(protoscope_wave_fft_layout_tests PROPERTIES TIMEOUT 60)
    add_executable(protoscope_file_operation_ui_tests test_file_operation_ui.cc)
    target_link_libraries(protoscope_file_operation_ui_tests PRIVATE protoscope_ui)
    add_test(NAME protoscope_file_operation_ui_tests COMMAND protoscope_file_operation_ui_tests)
    set_tests_properties(protoscope_file_operation_ui_tests PROPERTIES TIMEOUT 60)
    add_executable(protoscope_wave_ui_benchmark benchmark_wave_ui.cc)
    target_link_libraries(protoscope_wave_ui_benchmark PRIVATE protoscope_ui)
    add_test(NAME protoscope_wave_ui_multiframe COMMAND protoscope_wave_ui_benchmark --verify)
    set_tests_properties(protoscope_wave_ui_multiframe PROPERTIES TIMEOUT 60)
    add_executable(protoscope_tests
        test_main.cc
        test_registry.cc
        test_application.cc
        test_algorithm_help.cc
        test_codec.cc
        test_dock.cc
        test_dock_layout.cc
        test_frame_stream_parser.cc
        test_keyboard_shortcuts.cc
        test_plot.cc
        test_wave_view_transform.cc
        test_protocol_state_file.cc
        test_runtime_scheduler.cc
        test_startup_diagnostics.cc
        test_script_runtime_worker.cc
        test_elf_static_view_bridge.cc
        test_script_host.cc
        test_serial_ports.cc
        test_version_utils.cc
        test_update_check.cc
        test_transport.cc
        test_ui_theme.cc
    )
    target_include_directories(protoscope_tests
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_BINARY_DIR}/generated/include
    )
    target_link_libraries(protoscope_tests
        PRIVATE
            protoscope_config
            protoscope_dock
            protoscope_ui_layout
            protoscope_app
            protoscope_startup_diagnostics
            protoscope_elf_static_view
            protoscope_protocol_utils
            protoscope_plot
            protoscope_session
            protoscope_scripting
            protoscope_transport
            protoscope_ui
            protoscope_update_check
    )
    target_compile_features(protoscope_tests PRIVATE cxx_std_20)
    if(WIN32 AND MSVC)
        target_link_options(protoscope_tests PRIVATE "/DELAYLOAD:D3DCOMPILER_47.dll")
        target_link_libraries(protoscope_tests PRIVATE delayimp)
    endif()
    protoscope_enable_warnings(protoscope_tests)

    add_test(NAME protoscope_unit_tests COMMAND protoscope_tests)
    set_tests_properties(protoscope_unit_tests PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        # Windows Debug 全量单测包含应用层调度等待，显式放宽 CTest 级超时避免误报。
        TIMEOUT 900
    )
endif()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME protoscope_luals_api_manifest_check
        COMMAND ${Python3_EXECUTABLE} tools/generate_luals_api.py --check
    )
    set_tests_properties(protoscope_luals_api_manifest_check PROPERTIES WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
endif()
add_executable(protoscope_file_dialog_preferences_tests test_file_dialog_preferences.cc)
target_link_libraries(protoscope_file_dialog_preferences_tests PRIVATE protoscope_config)
add_test(NAME protoscope_file_dialog_preferences_tests COMMAND protoscope_file_dialog_preferences_tests)
set_tests_properties(protoscope_file_dialog_preferences_tests PROPERTIES TIMEOUT 60)
add_executable(protoscope_wave_status_tests test_wave_status.cc)
target_include_directories(protoscope_wave_status_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(protoscope_wave_status_tests PRIVATE cxx_std_20)
add_test(NAME protoscope_wave_status_tests COMMAND protoscope_wave_status_tests)
set_tests_properties(protoscope_wave_status_tests PROPERTIES TIMEOUT 60)

if(MSVC)
    # 测试和基准目标统一使用 UTF-8，避免中文注释及字符串受系统代码页影响。
    get_property(protoscope_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target_name IN LISTS protoscope_test_targets)
        target_compile_options(${target_name} PRIVATE /utf-8)
    endforeach()
endif()
