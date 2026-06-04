#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace protoscope::protocol_utils {

struct HexEditorNormalization {
    std::string text;
    std::size_t cursorPos{0};
    std::size_t digitCount{0};
};

std::string bytesToHex(const std::vector<std::uint8_t>& bytes, bool withSpace = true);
std::optional<std::vector<std::uint8_t>> hexToBytes(std::string_view text);
std::string normalizeHexText(std::string_view text);
HexEditorNormalization normalizeHexEditorInput(std::string_view text, std::size_t cursorPos);
std::size_t countHexDigits(std::string_view text);

std::uint16_t crc16Modbus(const std::vector<std::uint8_t>& bytes);
std::uint16_t crc16Modbus(const std::uint8_t* bytes, std::size_t count);
std::uint16_t crc16CcittFalse(const std::vector<std::uint8_t>& bytes);
std::uint16_t crc16CcittFalse(const std::uint8_t* bytes, std::size_t count);
std::uint32_t crc32Ieee(const std::vector<std::uint8_t>& bytes);
std::uint32_t crc32Ieee(const std::uint8_t* bytes, std::size_t count);

} // namespace protoscope::protocol_utils
