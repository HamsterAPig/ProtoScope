add_executable(protoscope_tests
    test_main.cpp
    test_application.cpp
    test_codec.cpp
    test_dock.cpp
    test_dock_layout.cpp
    test_frame_stream_parser.cpp
    test_plot.cpp
    test_protocol_state_file.cpp
    test_runtime_scheduler.cpp
    test_script_runtime_worker.cpp
    test_elf_static_view_bridge.cpp
    test_script_host.cpp
    test_serial_ports.cpp
    test_version_utils.cpp
    test_update_check.cpp
    test_transport.cpp
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
        protoscope_elf_static_view
        protoscope_protocol_utils
        protoscope_plot
        protoscope_scripting
        protoscope_transport
        protoscope_update_check
)
target_compile_features(protoscope_tests PRIVATE cxx_std_20)
protoscope_enable_warnings(protoscope_tests)

add_test(NAME protoscope_unit_tests COMMAND protoscope_tests)
set_tests_properties(protoscope_unit_tests PROPERTIES WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME protoscope_luals_api_manifest_check
        COMMAND ${Python3_EXECUTABLE} tools/generate_luals_api.py --check
    )
    set_tests_properties(protoscope_luals_api_manifest_check PROPERTIES WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
endif()
