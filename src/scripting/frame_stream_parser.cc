#include "protoscope/scripting/frame_stream_parser.hpp"

#include "protoscope/protocol_utils/codec.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <stdexcept>

namespace protoscope::scripting {

namespace {

    std::vector<std::uint8_t> copyBytes(const std::uint8_t* data, std::size_t count)
    {
        if (data == nullptr || count == 0) {
            return {};
        }
        return std::vector<std::uint8_t>(data, data + count);
    }

    struct StreamValueTypeInfo {
        StreamValueType type{StreamValueType::U8};
        std::string_view name{"u8"};
        std::size_t width{1};
        bool isFloat{false};
        bool isSigned{false};
        bool littleEndian{false};
    };

    constexpr std::array<StreamValueTypeInfo, 13> kStreamValueTypes{{
        {StreamValueType::U8, "u8", 1, false, false, false},
        {StreamValueType::I8, "i8", 1, false, true, false},
        {StreamValueType::U16Be, "u16_be", 2, false, false, false},
        {StreamValueType::U16Le, "u16_le", 2, false, false, true},
        {StreamValueType::I16Be, "i16_be", 2, false, true, false},
        {StreamValueType::I16Le, "i16_le", 2, false, true, true},
        {StreamValueType::U32Be, "u32_be", 4, false, false, false},
        {StreamValueType::U32Le, "u32_le", 4, false, false, true},
        {StreamValueType::I32Be, "i32_be", 4, false, true, false},
        {StreamValueType::I32Le, "i32_le", 4, false, true, true},
        {StreamValueType::F32Be, "f32_be", 4, true, false, false},
        {StreamValueType::F32Le, "f32_le", 4, true, false, true},
        {StreamValueType::Bytes, "bytes", 1, false, false, false},
    }};

    const StreamValueTypeInfo& streamValueTypeInfo(StreamValueType type)
    {
        for (const auto& info : kStreamValueTypes) {
            if (info.type == type) {
                return info;
            }
        }
        return kStreamValueTypes.front();
    }

    std::uint32_t readUnsignedValue(const std::uint8_t* bytes, std::size_t width, bool littleEndian)
    {
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < width; ++index) {
            const auto byte = static_cast<std::uint32_t>(bytes[littleEndian ? index : width - index - 1]);
            value |= byte << (index * 8U);
        }
        return value;
    }

    std::size_t streamCrcWidth(StreamCrcType type)
    {
        switch (type) {
            case StreamCrcType::Crc16Modbus:
            case StreamCrcType::Crc16CcittFalse:
                return 2;
            case StreamCrcType::Crc32Ieee:
                return 4;
            case StreamCrcType::None:
                return 0;
        }
        return 0;
    }

    std::uint32_t readCrcValue(const std::uint8_t* frameBytes,
                               std::size_t frameLength,
                               std::size_t crcWidth,
                               StreamCrcOrder order)
    {
        const auto start = frameLength - crcWidth;
        std::uint32_t value = 0;
        if (order == StreamCrcOrder::LoHi) {
            for (std::size_t index = 0; index < crcWidth; ++index) {
                value |= static_cast<std::uint32_t>(frameBytes[start + index]) << (index * 8U);
            }
            return value;
        }

        for (std::size_t index = 0; index < crcWidth; ++index) {
            value = (value << 8U) | frameBytes[start + index];
        }
        return value;
    }

    bool checkedAddSize(std::size_t left, std::size_t right, std::size_t& out)
    {
        if (left > (std::numeric_limits<std::size_t>::max)() - right) {
            return false;
        }
        out = left + right;
        return true;
    }

    bool checkedMultiplySize(std::size_t left, std::size_t right, std::size_t& out)
    {
        if (left != 0 && right > (std::numeric_limits<std::size_t>::max)() / left) {
            return false;
        }
        out = left * right;
        return true;
    }

    bool checkedSubtractInt64(std::int64_t left, std::int64_t right, std::int64_t& out)
    {
        const auto minValue = (std::numeric_limits<std::int64_t>::min)();
        const auto maxValue = (std::numeric_limits<std::int64_t>::max)();
        if ((right > 0 && left < minValue + right) || (right < 0 && left > maxValue + right)) {
            return false;
        }
        out = left - right;
        return true;
    }

    bool checkedMultiplyInt64(std::int64_t left, std::int64_t right, std::int64_t& out)
    {
        const auto minValue = (std::numeric_limits<std::int64_t>::min)();
        const auto maxValue = (std::numeric_limits<std::int64_t>::max)();
        if (left > 0) {
            if ((right > 0 && left > maxValue / right) || (right < 0 && right < minValue / left)) {
                return false;
            }
        } else if (left < 0) {
            if ((right > 0 && left < minValue / right) || (right < 0 && left < maxValue / right)) {
                return false;
            }
        }
        out = left * right;
        return true;
    }

    std::optional<std::int64_t> streamCountFieldValue(const StreamFieldMap& parsed,
                                                      const std::string& fieldName,
                                                      std::string& error)
    {
        const auto iter = parsed.find(fieldName);
        if (iter == parsed.end()) {
            error = "引用字段不存在: " + fieldName;
            return std::nullopt;
        }
        const auto value = iter->second.integerScalar();
        if (!value.has_value()) {
            error = "引用字段不是整数: " + fieldName;
            return std::nullopt;
        }
        return *value;
    }

    struct StreamCountEvaluationContext {
        const StreamFieldMap& parsed;
        std::size_t frameLength{0};
        std::size_t readableLimit{0};
        std::size_t fieldStart{0};
        std::size_t fieldWidth{0};
    };

    std::optional<std::int64_t> evaluateStreamCountExpression(const StreamCountExpression& expression,
                                                              const StreamCountEvaluationContext& context,
                                                              std::string& error);

    std::optional<std::int64_t> evaluateCountOperand(const StreamCountExpression& expression,
                                                     const StreamCountEvaluationContext& context,
                                                     std::string_view opName,
                                                     std::string& error)
    {
        if (!expression.operand) {
            error = std::string(opName) + " 缺少 operand";
            return std::nullopt;
        }
        return evaluateStreamCountExpression(*expression.operand, context, error);
    }

    std::optional<std::int64_t> evaluateCountArgument(const StreamCountExpression& expression,
                                                      const StreamCountEvaluationContext& context,
                                                      std::string& error)
    {
        if (!expression.argumentExpression) {
            return expression.argument;
        }
        return evaluateStreamCountExpression(*expression.argumentExpression, context, error);
    }

    std::optional<std::int64_t> evaluateDivCountExpression(const StreamCountExpression& expression,
                                                           const StreamCountEvaluationContext& context,
                                                           std::string& error)
    {
        const auto value = evaluateCountOperand(expression, context, "div", error);
        if (!value.has_value()) {
            return std::nullopt;
        }
        const auto argument = evaluateCountArgument(expression, context, error);
        if (!argument.has_value()) {
            return std::nullopt;
        }
        if (*argument == 0) {
            error = "div.by 不能为 0";
            return std::nullopt;
        }
        return *value / *argument;
    }

    std::optional<std::int64_t> evaluateSubCountExpression(const StreamCountExpression& expression,
                                                           const StreamCountEvaluationContext& context,
                                                           std::string& error)
    {
        const auto value = evaluateCountOperand(expression, context, "sub", error);
        if (!value.has_value()) {
            return std::nullopt;
        }
        std::int64_t result = 0;
        if (!checkedSubtractInt64(*value, expression.argument, result)) {
            error = "sub count 表达式溢出";
            return std::nullopt;
        }
        return result;
    }

    std::optional<std::int64_t> evaluateMulCountExpression(const StreamCountExpression& expression,
                                                           const StreamCountEvaluationContext& context,
                                                           std::string& error)
    {
        const auto value = evaluateCountOperand(expression, context, "mul", error);
        if (!value.has_value()) {
            return std::nullopt;
        }
        const auto argument = evaluateCountArgument(expression, context, error);
        if (!argument.has_value()) {
            return std::nullopt;
        }
        std::int64_t result = 0;
        if (!checkedMultiplyInt64(*value, *argument, result)) {
            error = "mul count 表达式溢出";
            return std::nullopt;
        }
        return result;
    }

    std::optional<std::int64_t> evaluateRemainingCountExpression(const StreamCountExpression& expression,
                                                                 const StreamCountEvaluationContext& context,
                                                                 std::string& error)
    {
        const auto limit = expression.excludeCrc ? context.readableLimit : context.frameLength;
        const auto unit = expression.argument > 0 ? static_cast<std::size_t>(expression.argument) : context.fieldWidth;
        if (unit == 0) {
            error = "remaining.unit 必须大于 0";
            return std::nullopt;
        }
        if (context.fieldStart > limit) {
            error = "remaining 起点超出帧边界";
            return std::nullopt;
        }
        return static_cast<std::int64_t>((limit - context.fieldStart) / unit);
    }

    std::optional<std::int64_t> evaluateIfFlagCountExpression(const StreamCountExpression& expression,
                                                              const StreamCountEvaluationContext& context,
                                                              std::string& error)
    {
        const auto value = streamCountFieldValue(context.parsed, expression.fieldName, error);
        if (!value.has_value()) {
            return std::nullopt;
        }
        const auto mask = static_cast<std::uint64_t>(expression.argument);
        const auto selected =
            (static_cast<std::uint64_t>(*value) & mask) != 0U ? expression.thenExpression : expression.elseExpression;
        if (!selected) {
            error = "if_flag 缺少 then/else 表达式";
            return std::nullopt;
        }
        return evaluateStreamCountExpression(*selected, context, error);
    }

    std::optional<std::int64_t> evaluateCaseCountExpression(const StreamCountExpression& expression,
                                                            const StreamCountEvaluationContext& context,
                                                            std::string& error)
    {
        const auto value = streamCountFieldValue(context.parsed, expression.fieldName, error);
        if (!value.has_value()) {
            return std::nullopt;
        }
        for (const auto& item : expression.cases) {
            if (item.value == *value && item.expression) {
                return evaluateStreamCountExpression(*item.expression, context, error);
            }
        }
        if (expression.defaultExpression) {
            return evaluateStreamCountExpression(*expression.defaultExpression, context, error);
        }
        error = "case 未匹配且没有 default";
        return std::nullopt;
    }

    std::optional<std::int64_t> evaluateBitCountExpression(const StreamCountExpression& expression,
                                                           const StreamCountEvaluationContext& context,
                                                           std::string& error)
    {
        const auto value = evaluateCountOperand(expression, context, "bit_count", error);
        if (!value.has_value()) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(std::popcount(static_cast<std::uint64_t>(*value)));
    }

    std::optional<std::int64_t> evaluateStreamCountExpression(const StreamCountExpression& expression,
                                                              const StreamCountEvaluationContext& context,
                                                              std::string& error)
    {
        switch (expression.op) {
            case StreamCountExpressionOp::Constant:
                return expression.value;
            case StreamCountExpressionOp::Field:
                return streamCountFieldValue(context.parsed, expression.fieldName, error);
            case StreamCountExpressionOp::Div:
                return evaluateDivCountExpression(expression, context, error);
            case StreamCountExpressionOp::Sub:
                return evaluateSubCountExpression(expression, context, error);
            case StreamCountExpressionOp::Mul:
                return evaluateMulCountExpression(expression, context, error);
            case StreamCountExpressionOp::Remaining:
                return evaluateRemainingCountExpression(expression, context, error);
            case StreamCountExpressionOp::IfFlag:
                return evaluateIfFlagCountExpression(expression, context, error);
            case StreamCountExpressionOp::Case:
                return evaluateCaseCountExpression(expression, context, error);
            case StreamCountExpressionOp::BitCount:
                return evaluateBitCountExpression(expression, context, error);
        }
        error = "未知 count 表达式";
        return std::nullopt;
    }

    std::optional<std::int64_t> evaluateStreamCountExpression(const StreamCountExpression& expression,
                                                              const StreamFieldMap& parsed,
                                                              std::size_t frameLength,
                                                              std::size_t readableLimit,
                                                              std::size_t fieldStart,
                                                              std::size_t fieldWidth,
                                                              std::string& error)
    {
        // 递归求值共享同一份上下文，避免每个操作分支重复传递边界参数。
        const StreamCountEvaluationContext context{
            .parsed = parsed,
            .frameLength = frameLength,
            .readableLimit = readableLimit,
            .fieldStart = fieldStart,
            .fieldWidth = fieldWidth,
        };
        return evaluateStreamCountExpression(expression, context, error);
    }

    std::optional<std::int64_t> decodeInteger(const std::uint8_t* raw,
                                              std::size_t size,
                                              std::size_t offset,
                                              StreamValueType type)
    {
        const auto& info = streamValueTypeInfo(type);
        if (info.isFloat || type == StreamValueType::Bytes || offset > size || info.width > size - offset) {
            return std::nullopt;
        }

        const auto rawValue = readUnsignedValue(raw + offset, info.width, info.littleEndian);
        if (!info.isSigned) {
            return static_cast<std::int64_t>(rawValue);
        }

        switch (info.width) {
            case 1:
                return static_cast<std::int8_t>(rawValue);
            case 2:
                return static_cast<std::int16_t>(rawValue);
            case 4:
                return static_cast<std::int32_t>(rawValue);
            default:
                return std::nullopt;
        }
    }

    std::optional<double> decodeFloat(const std::uint8_t* rawBytes,
                                      std::size_t size,
                                      std::size_t offset,
                                      StreamValueType type)
    {
        const auto& info = streamValueTypeInfo(type);
        if (!info.isFloat || offset > size || info.width > size - offset) {
            return std::nullopt;
        }
        const auto raw = readUnsignedValue(rawBytes + offset, info.width, info.littleEndian);
        return static_cast<double>(std::bit_cast<float>(raw));
    }

} // namespace

ByteRingBuffer::ByteRingBuffer(std::size_t capacity) : storage_(capacity, 0) {}

void ByteRingBuffer::reset()
{
    head_ = 0;
    size_ = 0;
}

void ByteRingBuffer::ensureCapacity(std::size_t capacity)
{
    if (capacity <= storage_.size()) {
        return;
    }
    std::vector<std::uint8_t> next(capacity, 0);
    for (std::size_t index = 0; index < size_; ++index) {
        next[index] = at(index);
    }
    storage_ = std::move(next);
    head_ = 0;
}

std::size_t ByteRingBuffer::capacity() const
{
    return storage_.size();
}

std::size_t ByteRingBuffer::size() const
{
    return size_;
}

bool ByteRingBuffer::empty() const
{
    return size_ == 0;
}

std::size_t ByteRingBuffer::append(const std::vector<std::uint8_t>& bytes, bool dropOldest)
{
    const std::size_t bufferCapacity = storage_.size();
    if (bufferCapacity == 0) {
        return bytes.size();
    }
    if (bytes.empty()) {
        return 0;
    }

    if (!dropOldest) {
        const std::size_t freeSpace = bufferCapacity - size_;
        const std::size_t accepted = (std::min)(freeSpace, bytes.size());
        appendContiguous(bytes.data(), accepted);
        return bytes.size() - accepted;
    }

    std::size_t dropped = 0;
    const std::uint8_t* source = bytes.data();
    std::size_t accepted = bytes.size();
    if (accepted >= bufferCapacity) {
        // 核心流程：输入块大于环形容量时，只保留最新一整窗，避免逐字节覆盖带来的取模开销。
        dropped = accepted - bufferCapacity + size_;
        source += accepted - bufferCapacity;
        accepted = bufferCapacity;
        head_ = 0;
        size_ = 0;
    } else if (accepted > bufferCapacity - size_) {
        dropped = accepted - (bufferCapacity - size_);
        discardFront(dropped);
    }

    appendContiguous(source, accepted);
    return dropped;
}

void ByteRingBuffer::appendContiguous(const std::uint8_t* bytes, std::size_t count)
{
    if (count == 0 || storage_.empty()) {
        return;
    }
    const std::size_t bufferCapacity = storage_.size();
    std::size_t tail = (head_ + size_) % bufferCapacity;
    const std::size_t firstCount = (std::min)(count, bufferCapacity - tail);
    std::copy_n(bytes, firstCount, storage_.begin() + static_cast<std::ptrdiff_t>(tail));
    if (firstCount < count) {
        std::copy_n(bytes + firstCount, count - firstCount, storage_.begin());
    }
    size_ = (std::min)(bufferCapacity, size_ + count);
}

void ByteRingBuffer::discardFront(std::size_t count)
{
    const auto actual = std::min(count, size_);
    if (actual == 0 || storage_.empty()) {
        return;
    }
    head_ = (head_ + actual) % storage_.size();
    size_ -= actual;
}

std::uint8_t ByteRingBuffer::at(std::size_t index) const
{
    if (index >= size_ || storage_.empty()) {
        throw std::out_of_range("ByteRingBuffer::at 越界");
    }
    return storage_[(head_ + index) % storage_.size()];
}

std::vector<std::uint8_t> ByteRingBuffer::slice(std::size_t offset, std::size_t count) const
{
    if (offset >= size_ || count == 0) {
        return {};
    }
    const auto actual = std::min(count, size_ - offset);
    std::vector<std::uint8_t> bytes;
    bytes.reserve(actual);
    for (std::size_t index = 0; index < actual; ++index) {
        bytes.push_back(at(offset + index));
    }
    return bytes;
}

ByteRingBuffer::LinearReadView ByteRingBuffer::linearRead(std::size_t offset,
                                                          std::size_t count,
                                                          std::vector<std::uint8_t>& scratch) const
{
    if (offset >= size_ || count == 0 || storage_.empty()) {
        return {};
    }
    const auto actual = std::min(count, size_ - offset);
    const auto start = (head_ + offset) % storage_.size();
    const auto contiguous = std::min(actual, storage_.size() - start);
    if (contiguous == actual) {
        return LinearReadView{.data = storage_.data() + start, .size = actual, .copied = false};
    }

    scratch.resize(actual);
    std::copy_n(storage_.data() + start, contiguous, scratch.data());
    std::copy_n(storage_.data(), actual - contiguous, scratch.data() + contiguous);
    return LinearReadView{.data = scratch.data(), .size = actual, .copied = true};
}

bool StreamFieldValue::isIntegerScalar() const
{
    return std::holds_alternative<std::int64_t>(value);
}

std::optional<std::int64_t> StreamFieldValue::integerScalar() const
{
    if (!std::holds_alternative<std::int64_t>(value)) {
        return std::nullopt;
    }
    return std::get<std::int64_t>(value);
}

FrameStreamParser::FrameStreamParser(StreamBufferDefinition buffer, std::vector<StreamFrameDefinition> frames)
    : bufferDefinition_(std::move(buffer)), frames_(std::move(frames)), buffer_(bufferDefinition_.capacity)
{
    buildCompiledFrames();
}

void FrameStreamParser::reset()
{
    buffer_.reset();
}

const StreamBufferDefinition& FrameStreamParser::bufferDefinition() const
{
    return bufferDefinition_;
}

const std::vector<StreamFrameDefinition>& FrameStreamParser::frameDefinitions() const
{
    return frames_;
}

void FrameStreamParser::clearRuntimeProfiles()
{
    runtimeProfiles_.clear();
}

bool FrameStreamParser::setRuntimeProfile(const std::string& frameName,
                                          StreamRuntimeProfile profile,
                                          std::string& error)
{
    const auto frameIter =
        std::find_if(frames_.begin(), frames_.end(), [&frameName](const StreamFrameDefinition& frame) {
            return frame.name == frameName;
        });
    if (frameIter == frames_.end()) {
        error = "未找到 stream frame: " + frameName;
        return false;
    }
    if (!frameIter->runtimeProfile) {
        error = "frame 未声明 runtime_profile: " + frameName;
        return false;
    }
    if (profile.length == 0) {
        error = "runtime profile length 必须大于 0";
        return false;
    }
    if (bufferDefinition_.capacity > 0 && profile.length > bufferDefinition_.capacity) {
        error = "runtime profile length 超出 stream buffer 容量";
        return false;
    }
    if (!profile.channelMap.empty()) {
        // 以实际出现的最大物理通道号确定去重数组大小，
        // 避免误判如 {1, 2}（物理通道 2/3）为越界。
        const auto maxTarget = *std::max_element(profile.channelMap.begin(), profile.channelMap.end());
        std::vector<bool> used(maxTarget + 1, false);
        for (const auto target : profile.channelMap) {
            if (used[target]) {
                error = "runtime profile channel_map 存在重复目标";
                return false;
            }
            used[target] = true;
        }
    }
    runtimeProfiles_.insert_or_assign(frameName, std::move(profile));
    return true;
}

bool FrameStreamParser::clearRuntimeProfile(const std::optional<std::string>& frameName, std::string& error)
{
    if (!frameName.has_value()) {
        runtimeProfiles_.clear();
        return true;
    }
    const auto erased = runtimeProfiles_.erase(*frameName);
    if (erased == 0U) {
        error = "runtime profile 不存在: " + *frameName;
        return false;
    }
    return true;
}

void FrameStreamParser::ensureAppendCapacity(std::size_t incomingSize)
{
    if (bufferDefinition_.dropOldest) {
        return;
    }

    const auto availableCapacity = (std::numeric_limits<std::size_t>::max)() - buffer_.size();
    const auto requiredCapacity = incomingSize > availableCapacity ? (std::numeric_limits<std::size_t>::max)()
                                                                   : buffer_.size() + incomingSize;
    if (requiredCapacity > buffer_.capacity() && bufferDefinition_.maxCapacity > buffer_.capacity()) {
        // 核心流程：默认无损模式下先按宿主预算扩容，避免小 Lua schema 容量直接触发丢帧。
        buffer_.ensureCapacity((std::min)(requiredCapacity, bufferDefinition_.maxCapacity));
    }
}

StreamParseBatch FrameStreamParser::appendIncomingBytes(const std::vector<std::uint8_t>& bytes)
{
    ensureAppendCapacity(bytes.size());

    const auto dropped = buffer_.append(bytes, bufferDefinition_.dropOldest);
    StreamParseBatch batch;
    batch.bufferSize = buffer_.size();
    batch.bufferCapacity = buffer_.capacity();
    batch.droppedBytes = dropped;
    batch.overflowed = dropped > 0;

    if (dropped > 0) {
        batch.errors.push_back(StreamParseError{
            .code = StreamParseErrorCode::Overflow,
            .message = "环形缓冲区超限，已丢弃最旧字节",
            .frameName = std::nullopt,
            .droppedBytes = dropped,
            .raw = {},
        });
    }

    updateNearOverflowStatus(batch);
    return batch;
}

void FrameStreamParser::updateNearOverflowStatus(StreamParseBatch& batch) const
{
    if (bufferDefinition_.nearOverflowNotify && batch.bufferCapacity > 0 && !batch.overflowed) {
        const auto ratio = static_cast<double>(batch.bufferSize) / static_cast<double>(batch.bufferCapacity);
        batch.nearOverflow = ratio >= bufferDefinition_.nearOverflowThresholdRatio;
    }
}

void FrameStreamParser::discardUnmatchedPrefix(StreamParseBatch& batch)
{
    const auto keep = maxHeaderLength() > 0 ? maxHeaderLength() - 1U : 0U;
    if (buffer_.size() <= keep) {
        return;
    }

    const auto droppedNoise = buffer_.size() - keep;
    buffer_.discardFront(droppedNoise);
    batch.errors.push_back(StreamParseError{
        .code = StreamParseErrorCode::NoiseDiscarded,
        .message = "未找到匹配帧头，已丢弃噪声前缀",
        .frameName = std::nullopt,
        .droppedBytes = droppedNoise,
        .raw = {},
    });
}

bool FrameStreamParser::discardCandidatePrefix(const CandidateMatch& candidate, StreamParseBatch& batch)
{
    if (candidate.start == 0) {
        return false;
    }

    buffer_.discardFront(candidate.start);
    batch.errors.push_back(StreamParseError{
        .code = StreamParseErrorCode::NoiseDiscarded,
        .message = "已跳过帧头前噪声字节",
        .frameName = std::nullopt,
        .droppedBytes = candidate.start,
        .raw = {},
    });
    return true;
}

FrameStreamParser::CandidateParseResult
FrameStreamParser::analyzeCandidateFrames(const CandidateMatch& candidate, const StreamParseOptions& options) const
{
    bool needMore = false;
    std::optional<StreamParseError> firstError;
    const auto window = ensureLinearWindow(buffer_.size());

    for (const auto index : *candidate.indexes) {
        const auto result = analyzeFrame(compiledFrames_[index], window, options);
        if (result.action == AnalyzeResult::Action::Parsed && result.frame.has_value()) {
            return CandidateParseResult{
                .action = CandidateParseResult::Action::Parsed,
                .frame = result.frame,
                .error = std::nullopt,
                .frameLength = result.frameLength,
            };
        }
        if (result.action == AnalyzeResult::Action::NeedMore) {
            needMore = true;
            continue;
        }
        if (!firstError.has_value() && result.error.has_value()) {
            firstError = result.error;
        }
    }

    if (needMore) {
        return CandidateParseResult{.action = CandidateParseResult::Action::NeedMore};
    }
    return CandidateParseResult{
        .action = CandidateParseResult::Action::RecoverableError,
        .frame = std::nullopt,
        .error = firstError,
        .frameLength = 0,
    };
}

StreamParseBatch FrameStreamParser::pushBytes(const std::vector<std::uint8_t>& bytes,
                                              const StreamParseOptions& options)
{
    if (bytes.empty()) {
        return StreamParseBatch{.bufferSize = buffer_.size(), .bufferCapacity = buffer_.capacity()};
    }

    auto batch = appendIncomingBytes(bytes);
    if (frames_.empty()) {
        return batch;
    }

    while (!buffer_.empty()) {
        const auto candidate = findCandidate();
        if (!candidate.has_value()) {
            discardUnmatchedPrefix(batch);
            break;
        }

        if (discardCandidatePrefix(*candidate, batch)) {
            continue;
        }

        const auto parseResult = analyzeCandidateFrames(*candidate, options);
        if (parseResult.action == CandidateParseResult::Action::Parsed && parseResult.frame.has_value()) {
            buffer_.discardFront(parseResult.frameLength);
            batch.frames.push_back(*parseResult.frame);
            continue;
        }
        if (parseResult.action == CandidateParseResult::Action::NeedMore) {
            break;
        }

        if (parseResult.error.has_value()) {
            batch.errors.push_back(*parseResult.error);
        }
        buffer_.discardFront(1);
    }

    batch.bufferSize = buffer_.size();
    return batch;
}

std::size_t FrameStreamParser::maxHeaderLength() const
{
    return maxHeaderLength_;
}

std::optional<FrameStreamParser::CandidateMatch> FrameStreamParser::findCandidate() const
{
    const auto window = ensureLinearWindow(buffer_.size());
    for (std::size_t start = 0; start < window.size; ++start) {
        const auto first = window.data[start];
        const auto& candidateIndexes = sortedHeaderFirstByteIndex_[first];
        if (candidateIndexes.empty()) {
            continue;
        }
        matchedCandidateIndexes_.clear();
        for (const auto index : candidateIndexes) {
            const auto& header = frames_[index].header;
            if (header.empty() || start + header.size() > buffer_.size()) {
                continue;
            }

            bool matched = true;
            for (std::size_t headerIndex = 1; headerIndex < header.size(); ++headerIndex) {
                if (window.data[start + headerIndex] != header[headerIndex]) {
                    matched = false;
                    break;
                }
            }

            if (matched) {
                matchedCandidateIndexes_.push_back(index);
            }
        }
        if (!matchedCandidateIndexes_.empty()) {
            return CandidateMatch{.start = start, .indexes = &matchedCandidateIndexes_};
        }
    }

    return std::nullopt;
}

void FrameStreamParser::setInvalidLengthError(const StreamFrameDefinition& frame,
                                              std::string_view message,
                                              AnalyzeResult& result) const
{
    result.action = AnalyzeResult::Action::RecoverableError;
    result.error = StreamParseError{
        .code = StreamParseErrorCode::InvalidLength,
        .message = std::string(message),
        .frameName = frame.name,
        .droppedBytes = 0,
        .raw = {},
    };
}

bool FrameStreamParser::resolveRuntimeProfileFrameLength(const StreamFrameDefinition& frame,
                                                         std::size_t& frameLength,
                                                         AnalyzeResult& result) const
{
    const auto profileIter = runtimeProfiles_.find(frame.name);
    if (profileIter == runtimeProfiles_.end()) {
        setInvalidLengthError(frame, "runtime_profile 帧缺少运行时长度", result);
        return false;
    }
    frameLength = profileIter->second.length;
    return true;
}

bool FrameStreamParser::resolveLengthFieldFrameLength(const StreamFrameDefinition& frame,
                                                      const ByteRingBuffer::LinearReadView& window,
                                                      std::size_t& frameLength,
                                                      AnalyzeResult& result) const
{
    const auto* frameBytes = window.data;
    if (frameBytes == nullptr || !frame.len.has_value()) {
        return false;
    }

    const auto width = streamValueWidth(frame.len->type);
    std::size_t lengthFieldEnd = 0;
    if (!checkedAddSize(frame.len->offset, width, lengthFieldEnd)) {
        setInvalidLengthError(frame, "长度字段 offset 溢出", result);
        return false;
    }
    if (lengthFieldEnd > window.size) {
        return false;
    }

    const auto parsedLength = decodeInteger(frameBytes, window.size, frame.len->offset, frame.len->type);
    if (!parsedLength.has_value() || *parsedLength < 0) {
        setInvalidLengthError(frame, "长度字段解析失败", result);
        return false;
    }

    if (frame.len->means == StreamLengthMeans::Payload) {
        if (!checkedAddSize(static_cast<std::size_t>(*parsedLength), frame.len->extra, frameLength)) {
            setInvalidLengthError(frame, "长度字段加成后溢出", result);
            return false;
        }
        return true;
    }

    frameLength = static_cast<std::size_t>(*parsedLength);
    return true;
}

bool FrameStreamParser::validateResolvedFrameLength(const StreamFrameDefinition& frame,
                                                    std::size_t frameLength,
                                                    AnalyzeResult& result) const
{
    if (frameLength == 0) {
        setInvalidLengthError(frame, "帧长度必须大于 0", result);
        return false;
    }

    if (frameLength > bufferDefinition_.capacity && bufferDefinition_.capacity > 0) {
        setInvalidLengthError(frame, "帧长度超过缓冲区容量", result);
        return false;
    }

    return true;
}

bool FrameStreamParser::resolveFrameLength(const StreamFrameDefinition& frame,
                                           const ByteRingBuffer::LinearReadView& window,
                                           std::size_t& frameLength,
                                           AnalyzeResult& result) const
{
    const auto* frameBytes = window.data;
    if (frameBytes == nullptr) {
        return false;
    }

    // 返回 false 表示窗口还不够，或 result 已经填充了可恢复错误。
    frameLength = 0;
    if (frame.runtimeProfile) {
        if (!resolveRuntimeProfileFrameLength(frame, frameLength, result)) {
            return false;
        }
    } else if (frame.size.has_value()) {
        frameLength = *frame.size;
    } else if (frame.len.has_value()) {
        if (!resolveLengthFieldFrameLength(frame, window, frameLength, result)) {
            return false;
        }
    }

    return validateResolvedFrameLength(frame, frameLength, result);
}

bool FrameStreamParser::validateFrameBounds(const StreamFrameDefinition& frame,
                                            const std::uint8_t* frameBytes,
                                            const ByteRingBuffer::LinearReadView& window,
                                            std::size_t frameLength,
                                            std::size_t crcWidth,
                                            AnalyzeResult& result) const
{
    if (frameLength > window.size) {
        return false;
    }

    if (frameLength < frame.header.size() + crcWidth) {
        result.action = AnalyzeResult::Action::RecoverableError;
        result.error = StreamParseError{
            .code = StreamParseErrorCode::InvalidLength,
            .message = "帧长度不足以容纳帧头与 CRC",
            .frameName = frame.name,
            .droppedBytes = 0,
            .raw = copyBytes(frameBytes, frameLength),
        };
        return false;
    }
    return true;
}

bool FrameStreamParser::validateFrameCrc(const StreamFrameDefinition& frame,
                                         const std::uint8_t* frameBytes,
                                         std::size_t frameLength,
                                         std::size_t crcWidth,
                                         AnalyzeResult& result) const
{
    if (crcWidth == 0) {
        return true;
    }

    std::uint32_t expected = 0;
    switch (frame.crc.type) {
        case StreamCrcType::Crc16Modbus:
            expected = protocol_utils::crc16Modbus(frameBytes, frameLength - crcWidth);
            break;
        case StreamCrcType::Crc16CcittFalse:
            expected = protocol_utils::crc16CcittFalse(frameBytes, frameLength - crcWidth);
            break;
        case StreamCrcType::Crc32Ieee:
            expected = protocol_utils::crc32Ieee(frameBytes, frameLength - crcWidth);
            break;
        case StreamCrcType::None:
            break;
    }

    const auto actual = readCrcValue(frameBytes, frameLength, crcWidth, frame.crc.order);
    if (expected == actual) {
        return true;
    }

    result.action = AnalyzeResult::Action::RecoverableError;
    result.error = StreamParseError{
        .code = StreamParseErrorCode::CrcMismatch,
        .message = "CRC 校验失败",
        .frameName = frame.name,
        .droppedBytes = 0,
        .raw = copyBytes(frameBytes, frameLength),
    };
    return false;
}

void FrameStreamParser::setCountResolveError(const StreamFrameDefinition& frame,
                                             const std::uint8_t* frameBytes,
                                             std::size_t frameLength,
                                             const std::string& countError,
                                             AnalyzeResult& result) const
{
    result.action = AnalyzeResult::Action::RecoverableError;
    result.error = StreamParseError{
        .code = StreamParseErrorCode::CountResolveFailed,
        .message = countError.empty() ? "字段数量解析失败" : countError,
        .frameName = frame.name,
        .droppedBytes = 0,
        .raw = copyBytes(frameBytes, frameLength),
    };
}

void FrameStreamParser::setFieldDecodeError(const StreamFrameDefinition& frame,
                                            const std::uint8_t* frameBytes,
                                            std::size_t frameLength,
                                            std::string_view message,
                                            AnalyzeResult& result) const
{
    result.action = AnalyzeResult::Action::RecoverableError;
    result.error = StreamParseError{
        .code = StreamParseErrorCode::FieldDecodeFailed,
        .message = std::string(message),
        .frameName = frame.name,
        .droppedBytes = 0,
        .raw = copyBytes(frameBytes, frameLength),
    };
}

bool FrameStreamParser::resolveFieldDecodePlan(const StreamFrameDefinition& frame,
                                               const StreamFieldDefinition& field,
                                               const std::uint8_t* frameBytes,
                                               std::size_t frameLength,
                                               std::size_t readableLimit,
                                               std::size_t cursor,
                                               const StreamFieldMap& parsedFields,
                                               FieldDecodePlan& plan,
                                               AnalyzeResult& result) const
{
    plan = FieldDecodePlan{
        .start = field.offset.value_or(cursor),
        .count = 0,
        .width = streamValueWidth(field.type),
        .end = field.offset.value_or(cursor),
    };

    std::string countError;
    const auto count = resolveFieldCount(field, parsedFields, frameLength, readableLimit, plan.start, countError);
    if (!count.has_value()) {
        setCountResolveError(frame, frameBytes, frameLength, countError, result);
        return false;
    }

    plan.count = *count;
    std::size_t fieldBytes = 0;
    if (!checkedMultiplySize(plan.count, plan.width, fieldBytes) ||
        !checkedAddSize(plan.start, fieldBytes, plan.end)) {
        setFieldDecodeError(frame, frameBytes, frameLength, "字段大小溢出", result);
        return false;
    }
    return true;
}

FrameStreamParser::FieldDecodeBoundsAction
FrameStreamParser::validateFieldDecodeBounds(const StreamFrameDefinition& frame,
                                             const std::uint8_t* frameBytes,
                                             std::size_t frameLength,
                                             std::size_t readableLimit,
                                             const FieldDecodePlan& plan,
                                             AnalyzeResult& result) const
{
    if (plan.start <= readableLimit && plan.end <= readableLimit) {
        return FieldDecodeBoundsAction::Decode;
    }

    // 运行时 profile 帧：超出帧边界的字段静默跳过，
    // 对应字段在 Lua 侧为 nil，由脚本侧兜底处理。
    if (frame.runtimeProfile) {
        return FieldDecodeBoundsAction::Skip;
    }

    // 静态帧：越界是硬错误
    setFieldDecodeError(frame, frameBytes, frameLength, "字段超出帧有效载荷范围", result);
    return FieldDecodeBoundsAction::Error;
}

void FrameStreamParser::decodeFieldValue(const StreamFieldDefinition& field,
                                         const std::uint8_t* frameBytes,
                                         std::size_t frameLength,
                                         const FieldDecodePlan& plan,
                                         StreamFieldMap& parsedFields) const
{
    if (field.type == StreamValueType::Bytes) {
        parsedFields[field.name] = StreamFieldValue{copyBytes(frameBytes + plan.start, plan.count)};
        return;
    }

    if (streamValueTypeIsFloat(field.type)) {
        if (plan.count == 1) {
            parsedFields[field.name] =
                StreamFieldValue{decodeFloat(frameBytes, frameLength, plan.start, field.type).value_or(0.0)};
            return;
        }

        std::vector<double> values;
        values.reserve(plan.count);
        for (std::size_t index = 0; index < plan.count; ++index) {
            values.push_back(
                decodeFloat(frameBytes, frameLength, plan.start + index * plan.width, field.type).value_or(0.0));
        }
        parsedFields[field.name] = StreamFieldValue{std::move(values)};
        return;
    }

    if (plan.count == 1) {
        parsedFields[field.name] =
            StreamFieldValue{decodeInteger(frameBytes, frameLength, plan.start, field.type).value_or(0)};
        return;
    }

    std::vector<std::int64_t> values;
    values.reserve(plan.count);
    for (std::size_t index = 0; index < plan.count; ++index) {
        values.push_back(
            decodeInteger(frameBytes, frameLength, plan.start + index * plan.width, field.type).value_or(0));
    }
    parsedFields[field.name] = StreamFieldValue{std::move(values)};
}

bool FrameStreamParser::decodeFrameFields(const StreamFrameDefinition& frame,
                                          const std::uint8_t* frameBytes,
                                          std::size_t frameLength,
                                          std::size_t crcWidth,
                                          StreamFieldMap& parsedFields,
                                          AnalyzeResult& result) const
{
    std::size_t cursor = 0;
    const auto readableLimit = frameLength - crcWidth;
    for (const auto& field : frame.fields) {
        FieldDecodePlan plan;
        if (!resolveFieldDecodePlan(
                frame, field, frameBytes, frameLength, readableLimit, cursor, parsedFields, plan, result)) {
            return false;
        }

        const auto boundsAction = validateFieldDecodeBounds(frame, frameBytes, frameLength, readableLimit, plan, result);
        if (boundsAction == FieldDecodeBoundsAction::Error) {
            return false;
        }
        if (boundsAction == FieldDecodeBoundsAction::Skip) {
            continue;
        }

        decodeFieldValue(field, frameBytes, frameLength, plan, parsedFields);
        cursor = std::max(cursor, plan.end);
    }
    return true;
}

FrameStreamParser::AnalyzeResult FrameStreamParser::analyzeFrame(const CompiledFrame& compiled,
                                                                 const ByteRingBuffer::LinearReadView& window,
                                                                 const StreamParseOptions& options) const
{
    AnalyzeResult result;
    const auto& frame = frames_[compiled.index];
    const auto* frameBytes = window.data;
    if (frameBytes == nullptr) {
        return result;
    }

    std::size_t frameLength = 0;
    if (!resolveFrameLength(frame, window, frameLength, result)) {
        return result;
    }

    const auto crcWidth = streamCrcWidth(frame.crc.type);
    if (!validateFrameBounds(frame, frameBytes, window, frameLength, crcWidth, result)) {
        return result;
    }

    if (!validateFrameCrc(frame, frameBytes, frameLength, crcWidth, result)) {
        return result;
    }

    StreamFieldMap parsedFields;
    parsedFields.reserve(frame.fields.size());
    if (!decodeFrameFields(frame, frameBytes, frameLength, crcWidth, parsedFields, result)) {
        return result;
    }

    result.action = AnalyzeResult::Action::Parsed;
    result.frameLength = frameLength;
    result.frame = StreamParsedFrame{
        .name = frame.name,
        .raw = options.includeFrameRaw ? copyBytes(frameBytes, frameLength) : std::vector<std::uint8_t>{},
        .fields = std::move(parsedFields),
        .crcOk = true,
        .channelMap = {},
    };
    std::string mapError;
    if (!applyRuntimeChannelMap(frame, *result.frame, mapError)) {
        result.action = AnalyzeResult::Action::RecoverableError;
        result.error = StreamParseError{
            .code = StreamParseErrorCode::FieldDecodeFailed,
            .message = mapError,
            .frameName = frame.name,
            .droppedBytes = 0,
            .raw = copyBytes(frameBytes, frameLength),
        };
        result.frame.reset();
        return result;
    }
    return result;
}

bool FrameStreamParser::applyRuntimeChannelMap(const StreamFrameDefinition& definition,
                                               StreamParsedFrame& frame,
                                               std::string& error) const
{
    if (!definition.runtimeProfile) {
        return true;
    }
    const auto profileIter = runtimeProfiles_.find(definition.name);
    if (profileIter == runtimeProfiles_.end()) {
        error = "runtime_profile 帧缺少 channel_map";
        return false;
    }
    frame.channelMap = profileIter->second.channelMap;
    (void)error;
    return true;
}

ByteRingBuffer::LinearReadView FrameStreamParser::ensureLinearWindow(std::size_t count) const
{
    return buffer_.linearRead(0, count, linearScratch_);
}

void FrameStreamParser::resetCompiledFrameIndexes()
{
    compiledFrames_.clear();
    compiledFrames_.reserve(frames_.size());
    maxHeaderLength_ = 0;
    for (auto& bucket : headerFirstByteIndex_) {
        bucket.clear();
    }
    for (auto& bucket : sortedHeaderFirstByteIndex_) {
        bucket.clear();
    }
}

FrameStreamParser::CompiledFrame
FrameStreamParser::compileFrameMetadata(std::size_t index, const StreamFrameDefinition& frame) const
{
    CompiledFrame compiled;
    compiled.index = index;
    compiled.hasHeader = !frame.header.empty();
    compiled.firstHeaderByte = compiled.hasHeader ? frame.header.front() : 0;
    compiled.minFrameLength = frame.header.size();

    applyDeclaredFrameLengthMinimum(frame, compiled);
    accumulateFixedFieldBytes(frame, compiled);
    applyFixedFieldMinimum(frame, compiled);
    return compiled;
}

void FrameStreamParser::applyDeclaredFrameLengthMinimum(const StreamFrameDefinition& frame,
                                                        CompiledFrame& compiled) const
{
    if (frame.size.has_value()) {
        compiled.minFrameLength = *frame.size;
        return;
    }

    if (frame.len.has_value()) {
        std::size_t lengthFieldEnd = 0;
        if (checkedAddSize(frame.len->offset, streamValueWidth(frame.len->type), lengthFieldEnd)) {
            compiled.minFrameLength = std::max(compiled.minFrameLength, lengthFieldEnd);
        } else {
            compiled.minFrameLength = (std::numeric_limits<std::size_t>::max)();
        }
    }
}

void FrameStreamParser::accumulateFixedFieldBytes(const StreamFrameDefinition& frame,
                                                  CompiledFrame& compiled) const
{
    for (const auto& field : frame.fields) {
        if (field.count.fixed.has_value()) {
            std::size_t fieldBytes = 0;
            if (!checkedMultiplySize(*field.count.fixed, streamValueWidth(field.type), fieldBytes) ||
                !checkedAddSize(compiled.fixedFieldBytes, fieldBytes, compiled.fixedFieldBytes)) {
                compiled.fixedFieldBytes = (std::numeric_limits<std::size_t>::max)();
                break;
            }
        }
    }
}

void FrameStreamParser::applyFixedFieldMinimum(const StreamFrameDefinition& frame, CompiledFrame& compiled) const
{
    std::size_t fixedMinimum = 0;
    if (checkedAddSize(frame.header.size(), compiled.fixedFieldBytes, fixedMinimum)) {
        compiled.minFrameLength = std::max(compiled.minFrameLength, fixedMinimum);
    } else {
        compiled.minFrameLength = (std::numeric_limits<std::size_t>::max)();
    }
}

void FrameStreamParser::registerCompiledFrame(const StreamFrameDefinition& frame, const CompiledFrame& compiled)
{
    maxHeaderLength_ = std::max(maxHeaderLength_, frame.header.size());
    compiledFrames_.push_back(compiled);
    if (compiled.hasHeader) {
        headerFirstByteIndex_[compiled.firstHeaderByte].push_back(compiled.index);
        sortedHeaderFirstByteIndex_[compiled.firstHeaderByte].push_back(compiled.index);
    }
}

void FrameStreamParser::sortCompiledFrameHeaderBuckets()
{
    for (auto& bucket : sortedHeaderFirstByteIndex_) {
        std::stable_sort(bucket.begin(), bucket.end(), [this](std::size_t left, std::size_t right) {
            return compiledFrames_[left].minFrameLength > compiledFrames_[right].minFrameLength;
        });
    }
}

void FrameStreamParser::buildCompiledFrames()
{
    resetCompiledFrameIndexes();
    for (std::size_t index = 0; index < frames_.size(); ++index) {
        const auto& frame = frames_[index];
        registerCompiledFrame(frame, compileFrameMetadata(index, frame));
    }
    sortCompiledFrameHeaderBuckets();
}

std::optional<std::size_t> FrameStreamParser::resolveFieldCount(const StreamFieldDefinition& field,
                                                                const StreamFieldMap& parsed,
                                                                std::size_t frameLength,
                                                                std::size_t readableLimit,
                                                                std::size_t fieldStart,
                                                                std::string& error) const
{
    if (field.count.fixed.has_value()) {
        return field.count.fixed;
    }
    if (field.count.fieldName.has_value()) {
        const auto iter = parsed.find(*field.count.fieldName);
        if (iter == parsed.end()) {
            error = "引用字段不存在";
            return std::nullopt;
        }
        const auto value = iter->second.integerScalar();
        if (!value.has_value() || *value < 0) {
            error = "引用字段不是非负整数";
            return std::nullopt;
        }
        return static_cast<std::size_t>(*value);
    }
    if (field.count.expression) {
        const auto value = evaluateStreamCountExpression(*field.count.expression,
                                                         parsed,
                                                         frameLength,
                                                         readableLimit,
                                                         fieldStart,
                                                         streamValueWidth(field.type),
                                                         error);
        if (!value.has_value()) {
            return std::nullopt;
        }
        if (*value < 0) {
            error = "count 表达式结果不能为负数";
            return std::nullopt;
        }
        if (static_cast<std::uint64_t>(*value) >
            static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
            error = "count 表达式结果过大";
            return std::nullopt;
        }
        return static_cast<std::size_t>(*value);
    }
    return 1U;
}

std::string_view streamParseErrorCodeName(StreamParseErrorCode code)
{
    switch (code) {
        case StreamParseErrorCode::Overflow:
            return "overflow";
        case StreamParseErrorCode::NoiseDiscarded:
            return "noise_discarded";
        case StreamParseErrorCode::InvalidLength:
            return "invalid_length";
        case StreamParseErrorCode::CrcMismatch:
            return "crc_mismatch";
        case StreamParseErrorCode::FieldDecodeFailed:
            return "field_decode_failed";
        case StreamParseErrorCode::CountResolveFailed:
            return "count_resolve_failed";
    }
    return "unknown";
}

std::optional<StreamValueType> streamValueTypeFromName(std::string_view name)
{
    for (const auto& info : kStreamValueTypes) {
        if (info.name == name) {
            return info.type;
        }
    }
    return std::nullopt;
}

std::string_view streamValueTypeName(StreamValueType type)
{
    return streamValueTypeInfo(type).name;
}

std::size_t streamValueWidth(StreamValueType type)
{
    return streamValueTypeInfo(type).width;
}

bool streamValueTypeIsFloat(StreamValueType type)
{
    return streamValueTypeInfo(type).isFloat;
}

bool streamValueTypeIsSigned(StreamValueType type)
{
    return streamValueTypeInfo(type).isSigned;
}

} // namespace protoscope::scripting
