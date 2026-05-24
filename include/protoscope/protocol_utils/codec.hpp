#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace protoscope::protocol_utils {

std::string bytesToHex(const std::vector<std::uint8_t>& bytes, bool withSpace = true);
std::optional<std::vector<std::uint8_t>> hexToBytes(std::string_view text);

std::uint16_t crc16Modbus(const std::vector<std::uint8_t>& bytes);
std::uint16_t crc16CcittFalse(const std::vector<std::uint8_t>& bytes);
std::uint32_t crc32Ieee(const std::vector<std::uint8_t>& bytes);

} // namespace protoscope::protocol_utils
