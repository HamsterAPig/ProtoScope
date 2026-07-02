add_executable(protoscope_tests
    test_main.cc
    test_application.cc
    test_codec.cc
    test_dock.cc
    test_dock_layout.cc
    test_frame_stream_parser.cc
    test_keyboard_shortcuts.cc
    test_plot.cc
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
)
target_include_directories(protoscope_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
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

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME protoscope_luals_api_manifest_check
        COMMAND ${Python3_EXECUTABLE} tools/generate_luals_api.py --check
    )
    set_tests_properties(protoscope_luals_api_manifest_check PROPERTIES WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
endif()
