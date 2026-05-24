#pragma once

using TestFn = void (*)();

struct TestCase {
    const char* name;
    TestFn run;
};

void test_hex_roundtrip();
void test_hex_invalid_input();
void test_hex_normalize_input();
void test_crc_known_vectors();
void test_script_controls_snapshot();
void test_script_read_version_flow();
void test_script_timeout_flow();
void test_protocol_directory_reload();
void test_config_default_roundtrip();
void test_tcp_transport_roundtrip();
void test_serial_transport_error_path();

const TestCase* allTests();
int testCount();
