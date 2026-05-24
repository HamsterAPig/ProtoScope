#pragma once

using TestFn = void (*)();

struct TestCase {
    const char* name;
    TestFn run;
};

void test_hex_roundtrip();
void test_hex_invalid_input();
void test_hex_normalize_input();
void test_hex_editor_cursor_normalize();
void test_crc_known_vectors();
void test_config_external_reload_state();
void test_script_controls_snapshot();
void test_script_on_open_log();
void test_script_on_close_log();
void test_script_on_error_log();
void test_script_multi_dock_snapshot();
void test_script_crc_bridge();
void test_script_read_version_flow();
void test_script_read_version_split_flow();
void test_script_timeout_flow();
void test_script_missing_callbacks_allowed();
void test_script_invalid_controls_fail();
void test_script_runtime_error_logged();
void test_protocol_directory_reload();
void test_config_default_roundtrip();
void test_config_default_script_workspace();
void test_tcp_transport_roundtrip();
void test_serial_transport_error_path();

const TestCase* allTests();
int testCount();
