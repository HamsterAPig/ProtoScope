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
void test_protocol_scan_and_root_roundtrip();
void test_dock_log_and_script_split();
void test_plot_history_trim_and_envelope();
void test_plot_cursor_snap_and_delta();
void test_plot_cursor_snap_by_time_and_measurement();
void test_wave_cursor_smart_snap_edge();
void test_wave_cursor_smart_snap_extreme();
void test_wave_cursor_smart_snap_fallback_to_nearest();
void test_plot_channel_offset_applies_to_display_only();
void test_plot_limited_envelope_edges();
void test_wave_frequency_parse_and_axis_mapping();
void test_wave_viewport_zoom_modes_and_clamp();
void test_wave_overview_viewport_normalize();
void test_wave_cursor_position_in_viewport();
void test_wave_cursor_interval_text_by_axis();
void test_wave_cursor_interval_lock();
void test_tcp_transport_roundtrip();
void test_transport_enqueue_send_async_roundtrip();
void test_tcp_server_connection_takeover_replaces_active_client();
void test_serial_transport_error_path();
void test_application_tcp_lua_read_version_roundtrip();
void test_application_lua_controls_without_connection();

const TestCase* allTests();
int testCount();
