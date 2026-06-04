#include "test_registry.hpp"

#include "protoscope/protocol_utils/codec.hpp"

#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
} // namespace

void test_hex_roundtrip() {
    const std::vector<std::uint8_t> input{0xAA, 0x01, 0x00, 0xFF};
    const auto hex = protoscope::protocol_utils::bytesToHex(input, true);
    require(hex == "AA 01 00 FF", "bytesToHex 输出不符合预期");

    const auto back = protoscope::protocol_utils::hexToBytes("aa 01 00 ff");
    require(back.has_value(), "hexToBytes 应成功");
    require(*back == input, "hexToBytes 回转不一致");
}

void test_hex_invalid_input() {
    const auto odd = protoscope::protocol_utils::hexToBytes("ABC");
    require(!odd.has_value(), "奇数长度应失败");

    const auto bad = protoscope::protocol_utils::hexToBytes("GG");
    require(!bad.has_value(), "非法字符应失败");
}

void test_hex_normalize_input() {
    const auto normalized = protoscope::protocol_utils::normalizeHexText(" aa0 10f ");
    require(normalized == "AA 01 0F", "HEX 文本归拢格式不符合预期");
}

void test_hex_editor_cursor_normalize() {
    const auto typed = protoscope::protocol_utils::normalizeHexEditorInput("5aa5", 4);
    require(typed.text == "5A A5", "逐字符输入归一化失败");
    require(typed.cursorPos == 5, "逐字符输入后的光标位置不正确");

    const auto pasted = protoscope::protocol_utils::normalizeHexEditorInput(" 5a-a5\n0f ", 8);
    require(pasted.text == "5A A5 0F", "混合分隔符粘贴归一化失败");
    require(pasted.digitCount == 6, "粘贴后的 nibble 统计不正确");
}

void test_crc_known_vectors() {
    const std::vector<std::uint8_t> text{'1', '2', '3', '4', '5', '6', '7', '8', '9'};

    const auto crc32 = protoscope::protocol_utils::crc32Ieee(text);
    require(crc32 == 0xCBF43926U, "CRC32 向量不匹配");

    const auto ccitt = protoscope::protocol_utils::crc16CcittFalse(text);
    require(ccitt == 0x29B1U, "CRC16-CCITT-FALSE 向量不匹配");

    const auto modbus = protoscope::protocol_utils::crc16Modbus(text);
    require(modbus == 0x4B37U, "CRC16-Modbus 向量不匹配");
}
