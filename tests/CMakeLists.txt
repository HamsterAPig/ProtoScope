add_executable(protoscope_tests
    test_main.cpp
    test_application.cpp
    test_codec.cpp
    test_dock.cpp
    test_dock_layout.cpp
    test_frame_stream_parser.cpp
    test_plot.cpp
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
