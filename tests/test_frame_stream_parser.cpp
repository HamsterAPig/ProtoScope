#include "test_registry.hpp"

#include "protoscope/protocol_utils/codec.hpp"
#include "protoscope/scripting/frame_stream_parser.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
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
    valuesField.count.fieldName = "count";

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

std::shared_ptr<protoscope::scripting::StreamCountExpression> countConst(std::int64_t value) {
    using namespace protoscope::scripting;
    auto expression = std::make_shared<StreamCountExpression>();
    expression->op = StreamCountExpressionOp::Constant;
    expression->value = value;
    return expression;
}

std::shared_ptr<protoscope::scripting::StreamCountExpression> countField(std::string fieldName) {
    using namespace protoscope::scripting;
    auto expression = std::make_shared<StreamCountExpression>();
    expression->op = StreamCountExpressionOp::Field;
    expression->fieldName = std::move(fieldName);
    return expression;
}

std::shared_ptr<protoscope::scripting::StreamCountExpression> countBinary(
    protoscope::scripting::StreamCountExpressionOp op,
    std::shared_ptr<protoscope::scripting::StreamCountExpression> operand,
    std::int64_t argument) {
    using namespace protoscope::scripting;
    auto expression = std::make_shared<StreamCountExpression>();
    expression->op = op;
    expression->operand = std::move(operand);
    expression->argument = argument;
    return expression;
}

protoscope::scripting::FrameStreamParser makeExpressionParser(
    const std::shared_ptr<protoscope::scripting::StreamCountExpression>& valuesCount,
    const std::vector<protoscope::scripting::StreamFieldDefinition>& prefixFields = {}) {
    using namespace protoscope::scripting;

    StreamFieldDefinition byteCount;
    byteCount.name = "byte_count";
    byteCount.type = StreamValueType::U8;
    byteCount.offset = 3;

    StreamFieldDefinition values;
    values.name = "values";
    values.type = StreamValueType::U16Be;
    values.offset = 4;
    values.count.expression = valuesCount;

    std::vector<StreamFieldDefinition> fields{byteCount};
    fields.insert(fields.end(), prefixFields.begin(), prefixFields.end());
    fields.push_back(std::move(values));

    StreamFrameDefinition frame;
    frame.name = "expression";
    frame.header = {0xAA, 0x55};
    frame.len = StreamLengthDefinition{
        .offset = 2,
        .type = StreamValueType::U8,
        .means = StreamLengthMeans::Payload,
        .extra = 5,
    };
    frame.crc = StreamCrcDefinition{.type = StreamCrcType::Crc16Modbus, .order = StreamCrcOrder::LoHi};
    frame.fields = std::move(fields);
    return FrameStreamParser(StreamBufferDefinition{.capacity = 64, .dropOldest = true}, {frame});
}

std::vector<std::int64_t> parseExpressionValues(protoscope::scripting::FrameStreamParser& parser,
                                                const std::vector<std::uint8_t>& payload) {
    const auto batch = parser.pushBytes(makeDynamicFrame(payload));
    require(batch.errors.empty(), "count 表达式不应解析失败");
    require(batch.frames.size() == 1, "count 表达式应解析出 1 帧");
    const auto& value = batch.frames[0].fields.at("values").value;
    if (const auto* items = std::get_if<std::vector<std::int64_t>>(&value); items != nullptr) {
        return *items;
    }
    if (const auto* item = std::get_if<std::int64_t>(&value); item != nullptr) {
        return {*item};
    }
    throw std::runtime_error("values 字段不是整数数组或整数标量");
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


void test_frame_stream_parser_large_chunk_keeps_latest_window() {
    using namespace protoscope::scripting;

    StreamFrameDefinition frame;
    frame.name = "fixed";
    frame.header = {0xFE};
    frame.size = 4;
    frame.crc = StreamCrcDefinition{.type = StreamCrcType::None};

    FrameStreamParser parser(StreamBufferDefinition{.capacity = 8, .dropOldest = true}, {frame});
    const auto batch = parser.pushBytes({0x20, 0x21, 0xFE, 0x01, 0x02, 0x03, 0xFE, 0x04, 0x05, 0x06});
    require(!batch.errors.empty(), "大块超过环形容量时应报告旧数据被覆盖");
    require(batch.errors.front().code == StreamParseErrorCode::Overflow, "大块覆盖错误码应为 overflow");
    require(batch.frames.size() == 2, "大块覆盖后应保留最新窗口内的完整帧");
    require(batch.frames.back().raw == std::vector<std::uint8_t>({0xFE, 0x04, 0x05, 0x06}), "保留窗口尾部应为最新帧");
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

void test_frame_stream_parser_count_expression_arithmetic() {
    using namespace protoscope::scripting;

    auto divParser = makeExpressionParser(countBinary(StreamCountExpressionOp::Div, countField("byte_count"), 2));
    auto divValues = parseExpressionValues(divParser, {0x04, 0x00, 0x11, 0x00, 0x22});
    require(divValues.size() == 2 && divValues[1] == 0x22, "div count 表达式应按字节数转寄存器数");

    auto subParser = makeExpressionParser(countBinary(StreamCountExpressionOp::Sub, countField("byte_count"), 2));
    auto subValues = parseExpressionValues(subParser, {0x04, 0x00, 0x11, 0x00, 0x22});
    require(subValues.size() == 2, "sub count 表达式应支持长度扣减");

    auto mulParser = makeExpressionParser(countBinary(StreamCountExpressionOp::Mul, countConst(1), 2));
    auto mulValues = parseExpressionValues(mulParser, {0x04, 0x00, 0x11, 0x00, 0x22});
    require(mulValues.size() == 2, "mul count 表达式应支持元素数转换");
}

void test_frame_stream_parser_count_expression_remaining_if_flag_and_case() {
    using namespace protoscope::scripting;

    auto remaining = std::make_shared<StreamCountExpression>();
    remaining->op = StreamCountExpressionOp::Remaining;
    remaining->argument = 2;
    remaining->excludeCrc = true;
    auto remainingParser = makeExpressionParser(remaining);
    auto remainingValues = parseExpressionValues(remainingParser, {0x04, 0x00, 0x11, 0x00, 0x22});
    require(remainingValues.size() == 2, "remaining count 表达式应解析到帧尾");

    StreamFieldDefinition flags;
    flags.name = "flags";
    flags.type = StreamValueType::U8;
    flags.offset = 3;
    auto ifFlag = std::make_shared<StreamCountExpression>();
    ifFlag->op = StreamCountExpressionOp::IfFlag;
    ifFlag->fieldName = "flags";
    ifFlag->argument = 0x01;
    ifFlag->thenExpression = countConst(1);
    ifFlag->elseExpression = countConst(0);
    auto ifParser = makeExpressionParser(ifFlag, {flags});
    auto ifValues = parseExpressionValues(ifParser, {0x01, 0x00, 0x66});
    require(ifValues.size() == 1 && ifValues[0] == 0x66, "if_flag count 表达式应按 flag 选择数量");

    StreamFieldDefinition func;
    func.name = "func";
    func.type = StreamValueType::U8;
    func.offset = 3;
    auto caseExpression = std::make_shared<StreamCountExpression>();
    caseExpression->op = StreamCountExpressionOp::Case;
    caseExpression->fieldName = "func";
    caseExpression->cases.push_back(StreamCountCase{.value = 0x03, .expression = countBinary(StreamCountExpressionOp::Div, countField("byte_count"), 2)});
    caseExpression->cases.push_back(StreamCountCase{.value = 0x10, .expression = countConst(2)});
    caseExpression->defaultExpression = countConst(1);
    auto caseParser = makeExpressionParser(caseExpression, {func});
    auto caseValues = parseExpressionValues(caseParser, {0x10, 0x00, 0x21, 0x00, 0x22});
    require(caseValues.size() == 2 && caseValues[0] == 0x21, "case count 表达式应按功能码选择数量");
}
