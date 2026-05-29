#include "test_registry.hpp"

#include "protoscope/protocol_utils/codec.hpp"
#include "protoscope/scripting/frame_stream_parser.hpp"

#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<std::uint8_t> makeDynamicFrame(const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> frame{0xAA, 0x55, static_cast<std::uint8_t>(payload.size())};
    frame.insert(frame.end(), payload.begin(), payload.end());
    const auto crc = protoscope::protocol_utils::crc16Modbus(frame);
    frame.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
    frame.push_back(static_cast<std::uint8_t>((crc >> 8U) & 0xFFU));
    return frame;
}

protoscope::scripting::FrameStreamParser makeDynamicParser(std::size_t capacity = 64) {
    using namespace protoscope::scripting;

    StreamFieldDefinition countField;
    countField.name = "count";
    countField.type = StreamValueType::U8;
    countField.offset = 3;

    StreamFieldDefinition valuesField;
    valuesField.name = "values";
    valuesField.type = StreamValueType::U8;
    valuesField.offset = 4;
    valuesField.count.callback = [](const StreamFieldMap& parsed,
                                    std::size_t,
                                    const std::vector<std::uint8_t>&,
                                    const std::string&,
                                    std::string&) -> std::optional<std::size_t> {
        const auto iter = parsed.find("count");
        if (iter == parsed.end()) {
            return std::nullopt;
        }
        const auto count = iter->second.integerScalar();
        if (!count.has_value()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(*count);
    };

    StreamFrameDefinition frame;
    frame.name = "dynamic";
    frame.header = {0xAA, 0x55};
    frame.len = StreamLengthDefinition{
        .offset = 2,
        .type = StreamValueType::U8,
        .means = StreamLengthMeans::Payload,
        .extra = 5,
    };
    frame.crc = StreamCrcDefinition{
        .type = StreamCrcType::Crc16Modbus,
        .order = StreamCrcOrder::LoHi,
    };
    frame.fields = {countField, valuesField};

    return FrameStreamParser(StreamBufferDefinition{.capacity = capacity, .dropOldest = true}, {frame});
}

} // namespace

void test_frame_stream_parser_waits_for_full_frame() {
    auto parser = makeDynamicParser();
    const auto frame = makeDynamicFrame({0x03, 0x07, 0x08, 0x09});

    const std::vector<std::uint8_t> first(frame.begin(), frame.begin() + 3);
    const std::vector<std::uint8_t> second(frame.begin() + 3, frame.end());

    auto batch = parser.pushBytes(first);
    require(batch.frames.empty(), "半包时不应提前产出 frame");
    require(batch.errors.empty(), "半包时不应产出错误");

    batch = parser.pushBytes(second);
    require(batch.frames.size() == 1, "补齐剩余字节后应产出 1 帧");
    const auto count = batch.frames[0].fields.at("count").integerScalar();
    require(count.has_value() && *count == 3, "count 字段解析错误");
    const auto values = std::get<std::vector<std::int64_t>>(batch.frames[0].fields.at("values").value);
    require(values.size() == 3, "动态 count 数组长度不正确");
    require(values[0] == 7 && values[1] == 8 && values[2] == 9, "动态 count 数组内容不正确");
}

void test_frame_stream_parser_handles_sticky_frames_and_noise_prefix() {
    auto parser = makeDynamicParser();
    auto first = makeDynamicFrame({0x01, 0x10});
    auto second = makeDynamicFrame({0x01, 0x20});

    std::vector<std::uint8_t> combined{0x00, 0xFF};
    combined.insert(combined.end(), first.begin(), first.end());
    combined.insert(combined.end(), second.begin(), second.end());

    const auto batch = parser.pushBytes(combined);
    require(batch.frames.size() == 2, "粘包时应连续解析出 2 帧");
    require(!batch.errors.empty(), "噪声前缀应记录错误");
    require(batch.errors.front().code == protoscope::scripting::StreamParseErrorCode::NoiseDiscarded, "噪声前缀错误码不正确");
}

void test_frame_stream_parser_crc_resync_keeps_following_frame() {
    auto parser = makeDynamicParser();
    auto broken = makeDynamicFrame({0x01, 0x33});
    auto good = makeDynamicFrame({0x01, 0x44});
    broken[broken.size() - 1] = static_cast<std::uint8_t>(broken.back() ^ 0x01U);

    std::vector<std::uint8_t> combined;
    combined.insert(combined.end(), broken.begin(), broken.end());
    combined.insert(combined.end(), good.begin(), good.end());

    const auto batch = parser.pushBytes(combined);
    require(batch.frames.size() == 1, "CRC 坏帧后仍应继续解析后续好帧");
    require(!batch.errors.empty(), "CRC 失败时应产出错误");
    require(batch.errors.front().code == protoscope::scripting::StreamParseErrorCode::CrcMismatch, "CRC 错误码不正确");
}

void test_frame_stream_parser_reports_overflow_drop_oldest() {
    auto parser = makeDynamicParser(6);
    const auto batch = parser.pushBytes({0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17});
    require(!batch.errors.empty(), "超出容量时应报告 overflow");
    require(batch.errors.front().code == protoscope::scripting::StreamParseErrorCode::Overflow, "overflow 错误码不正确");
    require(batch.errors.front().droppedBytes == 2, "overflow 丢弃字节数不正确");
}

void test_frame_stream_parser_supports_fixed_size_raw_frame() {
    using namespace protoscope::scripting;

    StreamFrameDefinition frame;
    frame.name = "fixed";
    frame.header = {0xFE};
    frame.size = 4;
    frame.crc = StreamCrcDefinition{.type = StreamCrcType::None};

    FrameStreamParser parser(StreamBufferDefinition{.capacity = 16, .dropOldest = true}, {frame});
    const auto batch = parser.pushBytes({0xFE, 0x01, 0x02, 0x03});
    require(batch.errors.empty(), "固定长度原始帧不应报错");
    require(batch.frames.size() == 1, "固定长度原始帧应成功切帧");
    require(batch.frames[0].raw.size() == 4, "固定长度原始帧长度不正确");
    require(batch.frames[0].fields.empty(), "无字段定义时不应生成 fields");
}
