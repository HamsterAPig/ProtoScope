#include "test_registry.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string_view>
#include <unordered_set>

namespace {

const TestCase kHeadlessTests[] = {
    {"hex_roundtrip", &test_hex_roundtrip},
    {"hex_invalid_input", &test_hex_invalid_input},
    {"crc_known_vectors", &test_crc_known_vectors},
    {"frame_stream_parser_waits_for_full_frame", &test_frame_stream_parser_waits_for_full_frame},
    {"frame_stream_parser_handles_sticky_frames_and_noise_prefix",
     &test_frame_stream_parser_handles_sticky_frames_and_noise_prefix},
    {"frame_stream_parser_runtime_profile_length_and_channel_map",
     &test_frame_stream_parser_runtime_profile_length_and_channel_map},
    {"headless_script_host_loads_default_protocol", &test_headless_script_host_loads_default_protocol},
    {"script_runtime_worker_disabled_mode_waits_for_rx_idle",
     &test_script_runtime_worker_disabled_mode_waits_for_rx_idle},
    {"script_runtime_worker_rx_limit_keeps_all_queued_bytes",
     &test_script_runtime_worker_rx_limit_keeps_all_queued_bytes},
    {"tcp_transport_roundtrip", &test_tcp_transport_roundtrip},
    {"udp_peer_transport_roundtrip", &test_udp_peer_transport_roundtrip},
    {"serial_port_name_normalization", &test_serial_port_name_normalization},
    {"headless_config_default_roundtrip", &test_headless_config_default_roundtrip},
    {"headless_raw_capture_file_roundtrip", &test_headless_raw_capture_file_roundtrip},
    {"headless_session_package_roundtrip_preserves_binary_entries",
     &test_headless_session_package_roundtrip_preserves_binary_entries},
    {"application_lua_controls_without_connection", &test_application_lua_controls_without_connection},
    {"application_failed_protocol_reload_keeps_previous_runtime",
     &test_application_failed_protocol_reload_keeps_previous_runtime},
    {"application_request_done_failure_sets_comm_error", &test_application_request_done_failure_sets_comm_error},
    {"application_initialize_prepares_default_config_and_protocol_dirs",
     &test_application_initialize_prepares_default_config_and_protocol_dirs},
};

} // namespace

int main()
{
    int failed = 0;
    const int total = static_cast<int>(sizeof(kHeadlessTests) / sizeof(kHeadlessTests[0]));
    std::unordered_set<std::string_view> testNames;
    for (int i = 0; i < total; ++i) {
        const std::string_view name{kHeadlessTests[i].name};
        if (!testNames.insert(name).second) {
            std::cerr << "[FAIL] duplicate test name: " << name << "\n";
            return 1;
        }
    }

    const char* filterEnv = std::getenv("PROTOSCOPE_TEST_FILTER");
    const std::string_view filter = filterEnv == nullptr ? std::string_view{} : std::string_view{filterEnv};
    int selected = 0;

    for (int i = 0; i < total; ++i) {
        const std::string_view name{kHeadlessTests[i].name};
        if (!filter.empty() && name.find(filter) == std::string_view::npos) {
            continue;
        }
        ++selected;
        try {
            kHeadlessTests[i].run();
            std::cout << "[PASS] " << kHeadlessTests[i].name << "\n" << std::flush;
        } catch (const std::exception& ex) {
            ++failed;
            std::cerr << "[FAIL] " << kHeadlessTests[i].name << ": " << ex.what() << "\n" << std::flush;
        } catch (...) {
            ++failed;
            std::cerr << "[FAIL] " << kHeadlessTests[i].name << ": unknown exception\n" << std::flush;
        }
    }

    std::cout << "total=" << total << " selected=" << selected << " failed=" << failed << "\n";
    return failed == 0 ? 0 : 1;
}
