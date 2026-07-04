#include "protoscope/plot/raw_capture_file.hpp"
#include "protoscope/session/session_package.hpp"

#include "test_helpers.hpp"
#include "test_registry.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

void test_headless_raw_capture_file_roundtrip()
{
    const protoscope::tests::ScopedTempPath tempFile(
        protoscope::tests::makeUniqueTempFile("protoscope-headless-raw-capture", ".psraw"));
    const auto& tempPath = tempFile.path();

    const protoscope::plot::RawCaptureFileData capture{
        .protocolName = "default_protocol",
        .protocolDir = "protocols/templates/default_protocol",
        .sampleFrequencyHz = 4096.0,
        .capturedAtMs = 123456789,
        .payload = {0x01, 0x02, 0x7F, 0x00, 0x41},
        .events = {},
    };

    std::string error;
    protoscope::tests::require(protoscope::plot::writeRawCaptureFile(tempPath, capture, error),
                               "headless psraw 写入应成功");
    const auto loaded = protoscope::plot::readRawCaptureFile(tempPath, error);
    if (!loaded.has_value()) {
        throw std::runtime_error("headless psraw 读回应成功: " + error);
    }
    protoscope::tests::require(loaded->protocolName == capture.protocolName, "headless psraw 应保留协议名");
    protoscope::tests::require(loaded->protocolDir == capture.protocolDir, "headless psraw 应保留协议目录");
    protoscope::tests::require(loaded->sampleFrequencyHz == capture.sampleFrequencyHz, "headless psraw 应保留采样频率");
    protoscope::tests::require(loaded->capturedAtMs == capture.capturedAtMs, "headless psraw 应保留采集时间");
    protoscope::tests::require(loaded->payload == capture.payload, "headless psraw 应保留原始 payload");
}

void test_headless_session_package_roundtrip_preserves_binary_entries()
{
    protoscope::session::SessionPackageData package{
        .createdAtMs = 123456,
        .entries =
            {
                {.name = "manifest.txt", .bytes = {'o', 'k', '\n'}},
                {.name = "capture.psraw", .bytes = {0x00, 0x01, static_cast<std::uint8_t>(0xFF), '\n'}},
            },
    };

    std::string error;
    const auto encoded = protoscope::session::encodeSessionPackage(package);
    const auto decoded = protoscope::session::decodeSessionPackage(encoded, error);
    if (!decoded.has_value()) {
        throw std::runtime_error("headless 会话包 roundtrip 应成功: " + error);
    }

    protoscope::tests::require(decoded->createdAtMs == package.createdAtMs, "headless 会话包 created_at_ms 应保留");
    const auto* manifest = protoscope::session::findSessionPackageEntry(*decoded, "manifest.txt");
    const auto* capture = protoscope::session::findSessionPackageEntry(*decoded, "capture.psraw");
    protoscope::tests::require(manifest != nullptr && manifest->bytes == package.entries[0].bytes,
                               "headless manifest 条目应保留");
    protoscope::tests::require(capture != nullptr && capture->bytes == package.entries[1].bytes,
                               "headless 二进制 capture 条目应保留");
}
