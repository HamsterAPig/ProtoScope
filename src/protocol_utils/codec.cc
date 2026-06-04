#include "protoscope/protocol_utils/codec.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>

namespace protoscope::protocol_utils {

std::string bytesToHex(const std::vector<std::uint8_t>& bytes, bool withSpace) {
    static constexpr std::array<char, 16> table{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    if (bytes.empty()) {
        return {};
    }

    std::string out;
    out.reserve(bytes.size() * (withSpace ? 3 : 2));
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto byte = bytes[i];
        out.push_back(table[(byte >> 4) & 0x0F]);
        out.push_back(table[byte & 0x0F]);
        if (withSpace && i + 1 < bytes.size()) {
            out.push_back(' ');
        }
    }
    return out;
}

namespace {
int hexNibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}
} // namespace

std::size_t countHexDigits(std::string_view text) {
    std::size_t count = 0;
    for (const char ch : text) {
        if (hexNibble(ch) >= 0) {
            ++count;
        }
    }
    return count;
}

std::optional<std::vector<std::uint8_t>> hexToBytes(std::string_view text) {
    std::string compact;
    compact.reserve(text.size());
    for (char ch : text) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            continue;
        }
        compact.push_back(ch);
    }

    if (compact.empty()) {
        return std::vector<std::uint8_t>{};
    }

    if (compact.size() % 2 != 0) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> out;
    out.reserve(compact.size() / 2);
    for (std::size_t i = 0; i < compact.size(); i += 2) {
        const int high = hexNibble(compact[i]);
        const int low = hexNibble(compact[i + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        out.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }

    return out;
}

std::string normalizeHexText(std::string_view text) {
    return normalizeHexEditorInput(text, text.size()).text;
}

HexEditorNormalization normalizeHexEditorInput(std::string_view text, std::size_t cursorPos) {
    std::string compact;
    compact.reserve(text.size());
    std::size_t digitsBeforeCursor = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char ch = text[index];
        if (std::isspace(static_cast<unsigned char>(ch))) {
            continue;
        }

        const int nibble = hexNibble(ch);
        if (nibble < 0) {
            continue;
        }

        if (index < cursorPos) {
            ++digitsBeforeCursor;
        }
        compact.push_back(nibble < 10 ? static_cast<char>('0' + nibble) : static_cast<char>('A' + (nibble - 10)));
    }

    if (compact.empty()) {
        return HexEditorNormalization{};
    }

    std::string normalized;
    normalized.reserve(compact.size() + compact.size() / 2);
    for (std::size_t i = 0; i < compact.size(); ++i) {
        normalized.push_back(compact[i]);
        if ((i % 2) == 1 && i + 1 < compact.size()) {
            normalized.push_back(' ');
        }
    }

    const auto clampedDigitsBeforeCursor = (std::min)(digitsBeforeCursor, compact.size());
    std::size_t normalizedCursor = clampedDigitsBeforeCursor;
    if (clampedDigitsBeforeCursor > 0) {
        normalizedCursor += (clampedDigitsBeforeCursor - 1) / 2;
    }

    return HexEditorNormalization{
        .text = std::move(normalized),
        .cursorPos = normalizedCursor,
        .digitCount = compact.size(),
    };
}

std::uint16_t crc16Modbus(const std::vector<std::uint8_t>& bytes) {
    return crc16Modbus(bytes.data(), bytes.size());
}

std::uint16_t crc16Modbus(const std::uint8_t* bytes, std::size_t count) {
    std::uint16_t crc = 0xFFFF;
    for (std::size_t index = 0; index < count; ++index) {
        const auto value = bytes[index];
        crc ^= static_cast<std::uint16_t>(value);
        for (int i = 0; i < 8; ++i) {
            const bool lsb = (crc & 0x0001U) != 0;
            crc >>= 1U;
            if (lsb) {
                crc ^= 0xA001U;
            }
        }
    }
    return crc;
}

std::uint16_t crc16CcittFalse(const std::vector<std::uint8_t>& bytes) {
    return crc16CcittFalse(bytes.data(), bytes.size());
}

std::uint16_t crc16CcittFalse(const std::uint8_t* bytes, std::size_t count) {
    std::uint16_t crc = 0xFFFF;
    for (std::size_t index = 0; index < count; ++index) {
        const auto value = bytes[index];
        crc ^= static_cast<std::uint16_t>(value << 8U);
        for (int i = 0; i < 8; ++i) {
            const bool msb = (crc & 0x8000U) != 0;
            crc <<= 1U;
            if (msb) {
                crc ^= 0x1021U;
            }
        }
    }
    return crc;
}

std::uint32_t crc32Ieee(const std::vector<std::uint8_t>& bytes) {
    return crc32Ieee(bytes.data(), bytes.size());
}

std::uint32_t crc32Ieee(const std::uint8_t* bytes, std::size_t count) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t index = 0; index < count; ++index) {
        const auto value = bytes[index];
        crc ^= static_cast<std::uint32_t>(value);
        for (int i = 0; i < 8; ++i) {
            const bool lsb = (crc & 1U) != 0;
            crc >>= 1U;
            if (lsb) {
                crc ^= 0xEDB88320U;
            }
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

} // namespace protoscope::protocol_utils
