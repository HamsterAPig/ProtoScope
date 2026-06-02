#include "protoscope/scripting/frame_stream_parser.hpp"

#include "protoscope/protocol_utils/codec.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace protoscope::scripting {

namespace {

std::vector<std::uint8_t> copyBytes(const std::uint8_t* data, std::size_t count) {
    if (data == nullptr || count == 0) {
        return {};
    }
    return std::vector<std::uint8_t>(data, data + count);
}

std::uint32_t readCrcValue(const std::uint8_t* frameBytes,
                           std::size_t frameLength,
                           std::size_t crcWidth,
                           StreamCrcOrder order) {
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

std::optional<std::int64_t> streamCountFieldValue(const StreamFieldMap& parsed,
                                                  const std::string& fieldName,
                                                  std::string& error) {
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

std::optional<std::int64_t> evaluateStreamCountExpression(const StreamCountExpression& expression,
                                                          const StreamFieldMap& parsed,
                                                          std::size_t frameLength,
                                                          std::size_t readableLimit,
                                                          std::size_t fieldStart,
                                                          std::size_t fieldWidth,
                                                          std::string& error) {
    switch (expression.op) {
    case StreamCountExpressionOp::Constant:
        return expression.value;
    case StreamCountExpressionOp::Field:
        return streamCountFieldValue(parsed, expression.fieldName, error);
    case StreamCountExpressionOp::Div: {
        if (!expression.operand) {
            error = "div 缺少 operand";
            return std::nullopt;
        }
        const auto value = evaluateStreamCountExpression(*expression.operand,
                                                         parsed,
                                                         frameLength,
                                                         readableLimit,
                                                         fieldStart,
                                                         fieldWidth,
                                                         error);
        if (!value.has_value()) {
            return std::nullopt;
        }
        std::optional<std::int64_t> argument = expression.argument;
        if (expression.argumentExpression) {
            argument = evaluateStreamCountExpression(*expression.argumentExpression,
                                                     parsed,
                                                     frameLength,
                                                     readableLimit,
                                                     fieldStart,
                                                     fieldWidth,
                                                     error);
            if (!argument.has_value()) {
                return std::nullopt;
            }
        }
        if (*argument == 0) {
            error = "div.by 不能为 0";
            return std::nullopt;
        }
        return *value / *argument;
    }
    case StreamCountExpressionOp::Sub: {
        if (!expression.operand) {
            error = "sub 缺少 operand";
            return std::nullopt;
        }
        const auto value = evaluateStreamCountExpression(*expression.operand,
                                                         parsed,
                                                         frameLength,
                                                         readableLimit,
                                                         fieldStart,
                                                         fieldWidth,
                                                         error);
        if (!value.has_value()) {
            return std::nullopt;
        }
        return *value - expression.argument;
    }
    case StreamCountExpressionOp::Mul: {
        if (!expression.operand) {
            error = "mul 缺少 operand";
            return std::nullopt;
        }
        const auto value = evaluateStreamCountExpression(*expression.operand,
                                                         parsed,
                                                         frameLength,
                                                         readableLimit,
                                                         fieldStart,
                                                         fieldWidth,
                                                         error);
        if (!value.has_value()) {
            return std::nullopt;
        }
        if (expression.argumentExpression) {
            const auto argument = evaluateStreamCountExpression(*expression.argumentExpression,
                                                                parsed,
                                                                frameLength,
                                                                readableLimit,
                                                                fieldStart,
                                                                fieldWidth,
                                                                error);
            if (!argument.has_value()) {
                return std::nullopt;
            }
            return *value * *argument;
        }
        return *value * expression.argument;
    }
    case StreamCountExpressionOp::Remaining: {
        const auto limit = expression.excludeCrc ? readableLimit : frameLength;
        const auto unit = expression.argument > 0 ? static_cast<std::size_t>(expression.argument) : fieldWidth;
        if (unit == 0) {
            error = "remaining.unit 必须大于 0";
            return std::nullopt;
        }
        if (fieldStart > limit) {
            error = "remaining 起点超出帧边界";
            return std::nullopt;
        }
        return static_cast<std::int64_t>((limit - fieldStart) / unit);
    }
    case StreamCountExpressionOp::IfFlag: {
        const auto value = streamCountFieldValue(parsed, expression.fieldName, error);
        if (!value.has_value()) {
            return std::nullopt;
        }
        const auto mask = static_cast<std::uint64_t>(expression.argument);
        const auto selected = (static_cast<std::uint64_t>(*value) & mask) != 0U
            ? expression.thenExpression
            : expression.elseExpression;
        if (!selected) {
            error = "if_flag 缺少 then/else 表达式";
            return std::nullopt;
        }
        return evaluateStreamCountExpression(*selected,
                                             parsed,
                                             frameLength,
                                             readableLimit,
                                             fieldStart,
                                             fieldWidth,
                                             error);
    }
    case StreamCountExpressionOp::Case: {
        const auto value = streamCountFieldValue(parsed, expression.fieldName, error);
        if (!value.has_value()) {
            return std::nullopt;
        }
        for (const auto& item : expression.cases) {
            if (item.value == *value && item.expression) {
                return evaluateStreamCountExpression(*item.expression,
                                                     parsed,
                                                     frameLength,
                                                     readableLimit,
                                                     fieldStart,
                                                     fieldWidth,
                                                     error);
            }
        }
        if (expression.defaultExpression) {
            return evaluateStreamCountExpression(*expression.defaultExpression,
                                                 parsed,
                                                 frameLength,
                                                 readableLimit,
                                                 fieldStart,
                                                 fieldWidth,
                                                 error);
        }
        error = "case 未匹配且没有 default";
        return std::nullopt;
    }
    case StreamCountExpressionOp::BitCount: {
        if (!expression.operand) {
            error = "bit_count 缺少 operand";
            return std::nullopt;
        }
        const auto value = evaluateStreamCountExpression(*expression.operand,
                                                         parsed,
                                                         frameLength,
                                                         readableLimit,
                                                         fieldStart,
                                                         fieldWidth,
                                                         error);
        if (!value.has_value()) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(std::popcount(static_cast<std::uint64_t>(*value)));
    }
    }
    error = "未知 count 表达式";
    return std::nullopt;
}

std::optional<std::int64_t> decodeInteger(const std::vector<std::uint8_t>& bytes,
                                          std::size_t offset,
                                          StreamValueType type) {
    const auto* raw = bytes.data();
    const auto size = bytes.size();
    const auto require = [size, offset](std::size_t width) -> bool {
        return offset + width <= size;
    };

    switch (type) {
    case StreamValueType::U8:
        return require(1) ? std::optional<std::int64_t>(raw[offset]) : std::nullopt;
    case StreamValueType::I8:
        return require(1) ? std::optional<std::int64_t>(static_cast<std::int8_t>(raw[offset])) : std::nullopt;
    case StreamValueType::U16Be:
        if (!require(2)) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(
            (static_cast<std::uint16_t>(raw[offset]) << 8U)
            | static_cast<std::uint16_t>(raw[offset + 1]));
    case StreamValueType::U16Le:
        if (!require(2)) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(
            static_cast<std::uint16_t>(raw[offset])
            | (static_cast<std::uint16_t>(raw[offset + 1]) << 8U));
    case StreamValueType::I16Be:
        if (!require(2)) {
            return std::nullopt;
        }
        return static_cast<std::int16_t>(
            (static_cast<std::uint16_t>(raw[offset]) << 8U)
            | static_cast<std::uint16_t>(raw[offset + 1]));
    case StreamValueType::I16Le:
        if (!require(2)) {
            return std::nullopt;
        }
        return static_cast<std::int16_t>(
            static_cast<std::uint16_t>(raw[offset])
            | (static_cast<std::uint16_t>(raw[offset + 1]) << 8U));
    case StreamValueType::U32Be:
        if (!require(4)) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(
            (static_cast<std::uint32_t>(raw[offset]) << 24U)
            | (static_cast<std::uint32_t>(raw[offset + 1]) << 16U)
            | (static_cast<std::uint32_t>(raw[offset + 2]) << 8U)
            | static_cast<std::uint32_t>(raw[offset + 3]));
    case StreamValueType::U32Le:
        if (!require(4)) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(
            static_cast<std::uint32_t>(raw[offset])
            | (static_cast<std::uint32_t>(raw[offset + 1]) << 8U)
            | (static_cast<std::uint32_t>(raw[offset + 2]) << 16U)
            | (static_cast<std::uint32_t>(raw[offset + 3]) << 24U));
    case StreamValueType::I32Be:
        if (!require(4)) {
            return std::nullopt;
        }
        return static_cast<std::int32_t>(
            (static_cast<std::uint32_t>(raw[offset]) << 24U)
            | (static_cast<std::uint32_t>(raw[offset + 1]) << 16U)
            | (static_cast<std::uint32_t>(raw[offset + 2]) << 8U)
            | static_cast<std::uint32_t>(raw[offset + 3]));
    case StreamValueType::I32Le:
        if (!require(4)) {
            return std::nullopt;
        }
        return static_cast<std::int32_t>(
            static_cast<std::uint32_t>(raw[offset])
            | (static_cast<std::uint32_t>(raw[offset + 1]) << 8U)
            | (static_cast<std::uint32_t>(raw[offset + 2]) << 16U)
            | (static_cast<std::uint32_t>(raw[offset + 3]) << 24U));
    case StreamValueType::F32Be:
    case StreamValueType::F32Le:
    case StreamValueType::Bytes:
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<double> decodeFloat(const std::vector<std::uint8_t>& bytes,
                                  std::size_t offset,
                                  StreamValueType type) {
    if (offset + 4 > bytes.size()) {
        return std::nullopt;
    }
    const auto* rawBytes = bytes.data();
    std::uint32_t raw = 0;
    switch (type) {
    case StreamValueType::F32Be:
        raw = (static_cast<std::uint32_t>(rawBytes[offset]) << 24U)
            | (static_cast<std::uint32_t>(rawBytes[offset + 1]) << 16U)
            | (static_cast<std::uint32_t>(rawBytes[offset + 2]) << 8U)
            | static_cast<std::uint32_t>(rawBytes[offset + 3]);
        return static_cast<double>(std::bit_cast<float>(raw));
    case StreamValueType::F32Le:
        raw = static_cast<std::uint32_t>(rawBytes[offset])
            | (static_cast<std::uint32_t>(rawBytes[offset + 1]) << 8U)
            | (static_cast<std::uint32_t>(rawBytes[offset + 2]) << 16U)
            | (static_cast<std::uint32_t>(rawBytes[offset + 3]) << 24U);
        return static_cast<double>(std::bit_cast<float>(raw));
    default:
        return std::nullopt;
    }
}

} // namespace

ByteRingBuffer::ByteRingBuffer(std::size_t capacity)
    : storage_(capacity, 0) {}

void ByteRingBuffer::reset() {
    head_ = 0;
    size_ = 0;
}

std::size_t ByteRingBuffer::capacity() const {
    return storage_.size();
}

std::size_t ByteRingBuffer::size() const {
    return size_;
}

bool ByteRingBuffer::empty() const {
    return size_ == 0;
}

std::size_t ByteRingBuffer::append(const std::vector<std::uint8_t>& bytes, bool dropOldest) {
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


void ByteRingBuffer::appendContiguous(const std::uint8_t* bytes, std::size_t count) {
    if (count == 0 || storage_.empty()) {
        return;
    }
    const std::size_t bufferCapacity = storage_.size();
    std::size_t tail = (head_ + size_) % bufferCapacity;
    const std::size_t firstCount = (std::min)(count, bufferCapacity - tail);
    std::copy_n(bytes, firstCount, storage_.begin() + static_cast<std::ptrdiff_t>(tail));
    if (firstCount < count) {
        std::copy_n(bytes + firstCount,
                    count - firstCount,
                    storage_.begin());
    }
    size_ = (std::min)(bufferCapacity, size_ + count);
}

void ByteRingBuffer::discardFront(std::size_t count) {
    const auto actual = std::min(count, size_);
    if (actual == 0 || storage_.empty()) {
        return;
    }
    head_ = (head_ + actual) % storage_.size();
    size_ -= actual;
}

std::uint8_t ByteRingBuffer::at(std::size_t index) const {
    if (index >= size_ || storage_.empty()) {
        throw std::out_of_range("ByteRingBuffer::at 越界");
    }
    return storage_[(head_ + index) % storage_.size()];
}

std::vector<std::uint8_t> ByteRingBuffer::slice(std::size_t offset, std::size_t count) const {
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
                                                          std::vector<std::uint8_t>& scratch) const {
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

bool StreamFieldValue::isIntegerScalar() const {
    return std::holds_alternative<std::int64_t>(value);
}

std::optional<std::int64_t> StreamFieldValue::integerScalar() const {
    if (!std::holds_alternative<std::int64_t>(value)) {
        return std::nullopt;
    }
    return std::get<std::int64_t>(value);
}

FrameStreamParser::FrameStreamParser(StreamBufferDefinition buffer, std::vector<StreamFrameDefinition> frames)
    : bufferDefinition_(std::move(buffer))
    , frames_(std::move(frames))
    , buffer_(bufferDefinition_.capacity) {
    buildCompiledFrames();
}

void FrameStreamParser::reset() {
    buffer_.reset();
}

const StreamBufferDefinition& FrameStreamParser::bufferDefinition() const {
    return bufferDefinition_;
}

const std::vector<StreamFrameDefinition>& FrameStreamParser::frameDefinitions() const {
    return frames_;
}

void FrameStreamParser::clearRuntimeProfiles() {
    runtimeProfiles_.clear();
}

bool FrameStreamParser::setRuntimeProfile(const std::string& frameName,
                                          StreamRuntimeProfile profile,
                                          std::string& error) {
    const auto frameIter = std::find_if(frames_.begin(), frames_.end(), [&frameName](const StreamFrameDefinition& frame) {
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
    if (!profile.channelMap.empty()) {
        std::vector<bool> used(profile.channelMap.size(), false);
        for (const auto target : profile.channelMap) {
            if (target >= profile.channelMap.size()) {
                error = "runtime profile channel_map 存在越界目标";
                return false;
            }
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

bool FrameStreamParser::clearRuntimeProfile(const std::optional<std::string>& frameName, std::string& error) {
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

StreamParseBatch FrameStreamParser::pushBytes(const std::vector<std::uint8_t>& bytes) {
    StreamParseBatch batch;
    batch.bufferCapacity = buffer_.capacity();
    if (bytes.empty()) {
        batch.bufferSize = buffer_.size();
        return batch;
    }

    const auto dropped = buffer_.append(bytes, bufferDefinition_.dropOldest);
    batch.bufferSize = buffer_.size();
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

    if (bufferDefinition_.nearOverflowNotify && batch.bufferCapacity > 0 && !batch.overflowed) {
        const auto ratio = static_cast<double>(batch.bufferSize) / static_cast<double>(batch.bufferCapacity);
        batch.nearOverflow = ratio >= bufferDefinition_.nearOverflowThresholdRatio;
    }

    if (frames_.empty()) {
        return batch;
    }

    while (!buffer_.empty()) {
        const auto candidate = findCandidate();
        if (!candidate.has_value()) {
            const auto keep = maxHeaderLength() > 0 ? maxHeaderLength() - 1U : 0U;
            if (buffer_.size() > keep) {
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
            break;
        }

        if (candidate->start > 0) {
            buffer_.discardFront(candidate->start);
            batch.errors.push_back(StreamParseError{
                .code = StreamParseErrorCode::NoiseDiscarded,
                .message = "已跳过帧头前噪声字节",
                .frameName = std::nullopt,
                .droppedBytes = candidate->start,
                .raw = {},
            });
            continue;
        }

        bool needMore = false;
        bool parsed = false;
        std::optional<StreamParseError> firstError;
        std::vector<std::size_t> indexes = candidate->indexes;
        std::stable_sort(indexes.begin(), indexes.end(), [this](std::size_t left, std::size_t right) {
            return compiledFrames_[left].minFrameLength > compiledFrames_[right].minFrameLength;
        });
        const auto window = ensureLinearWindow(buffer_.size());

        for (const auto index : indexes) {
            const auto result = analyzeFrame(compiledFrames_[index], window);
            if (result.action == AnalyzeResult::Action::Parsed && result.frame.has_value()) {
                buffer_.discardFront(result.frameLength);
                batch.frames.push_back(*result.frame);
                firstError.reset();
                needMore = false;
                parsed = true;
                break;
            }
            if (result.action == AnalyzeResult::Action::NeedMore) {
                needMore = true;
                continue;
            }
            if (!firstError.has_value() && result.error.has_value()) {
                firstError = result.error;
            }
        }

        if (parsed) {
            continue;
        }

        if (needMore) {
            break;
        }

        if (firstError.has_value()) {
            batch.errors.push_back(*firstError);
        }
        buffer_.discardFront(1);
    }

    batch.bufferSize = buffer_.size();
    return batch;
}

std::size_t FrameStreamParser::maxHeaderLength() const {
    return maxHeaderLength_;
}

std::optional<FrameStreamParser::CandidateMatch> FrameStreamParser::findCandidate() const {
    for (std::size_t start = 0; start < buffer_.size(); ++start) {
        CandidateMatch match;
        match.start = start;
        const auto first = buffer_.at(start);
        const auto& candidateIndexes = headerFirstByteIndex_[first];
        for (const auto index : candidateIndexes) {
            const auto& header = frames_[index].header;
            if (header.empty() || start + header.size() > buffer_.size()) {
                continue;
            }

            bool matched = true;
            for (std::size_t headerIndex = 1; headerIndex < header.size(); ++headerIndex) {
                if (buffer_.at(start + headerIndex) != header[headerIndex]) {
                    matched = false;
                    break;
                }
            }

            if (matched) {
                match.indexes.push_back(index);
            }
        }

        if (!match.indexes.empty()) {
            return match;
        }
    }

    return std::nullopt;
}

FrameStreamParser::AnalyzeResult FrameStreamParser::analyzeFrame(const CompiledFrame& compiled,
                                                                 const ByteRingBuffer::LinearReadView& window) const {
    AnalyzeResult result;
    const auto& frame = frames_[compiled.index];
    const auto* frameBytes = window.data;
    if (frameBytes == nullptr) {
        return result;
    }

    std::size_t frameLength = 0;
    if (frame.runtimeProfile) {
        const auto profileIter = runtimeProfiles_.find(frame.name);
        if (profileIter == runtimeProfiles_.end()) {
            result.action = AnalyzeResult::Action::RecoverableError;
            result.error = StreamParseError{
                .code = StreamParseErrorCode::InvalidLength,
                .message = "runtime_profile 帧缺少运行时长度",
                .frameName = frame.name,
                .droppedBytes = 0,
                .raw = {},
            };
            return result;
        }
        frameLength = profileIter->second.length;
    } else if (frame.size.has_value()) {
        frameLength = *frame.size;
    } else if (frame.len.has_value()) {
        const auto width = streamValueWidth(frame.len->type);
        if (frame.len->offset + width > window.size) {
            return result;
        }
        const auto lengthBytes = copyBytes(frameBytes + frame.len->offset, width);
        const auto parsedLength = decodeInteger(lengthBytes, 0, frame.len->type);
        if (!parsedLength.has_value() || *parsedLength < 0) {
            result.action = AnalyzeResult::Action::RecoverableError;
            result.error = StreamParseError{
                .code = StreamParseErrorCode::InvalidLength,
                .message = "长度字段解析失败",
                .frameName = frame.name,
                .droppedBytes = 0,
                .raw = {},
            };
            return result;
        }

        if (frame.len->means == StreamLengthMeans::Payload) {
            frameLength = static_cast<std::size_t>(*parsedLength) + frame.len->extra;
        } else {
            frameLength = static_cast<std::size_t>(*parsedLength);
        }
    }

    if (frameLength == 0) {
        result.action = AnalyzeResult::Action::RecoverableError;
        result.error = StreamParseError{
            .code = StreamParseErrorCode::InvalidLength,
            .message = "帧长度必须大于 0",
            .frameName = frame.name,
            .droppedBytes = 0,
            .raw = {},
        };
        return result;
    }

    if (frameLength > bufferDefinition_.capacity && bufferDefinition_.capacity > 0) {
        result.action = AnalyzeResult::Action::RecoverableError;
        result.error = StreamParseError{
            .code = StreamParseErrorCode::InvalidLength,
            .message = "帧长度超过缓冲区容量",
            .frameName = frame.name,
            .droppedBytes = 0,
            .raw = {},
        };
        return result;
    }

    if (frameLength > window.size) {
        return result;
    }

    std::size_t crcWidth = 0;
    if (frame.crc.type == StreamCrcType::Crc16Modbus || frame.crc.type == StreamCrcType::Crc16CcittFalse) {
        crcWidth = 2;
    } else if (frame.crc.type == StreamCrcType::Crc32Ieee) {
        crcWidth = 4;
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
        return result;
    }

    if (crcWidth > 0) {
        const auto crcData = copyBytes(frameBytes, frameLength - crcWidth);
        std::uint32_t expected = 0;
        switch (frame.crc.type) {
        case StreamCrcType::Crc16Modbus:
            expected = protocol_utils::crc16Modbus(crcData);
            break;
        case StreamCrcType::Crc16CcittFalse:
            expected = protocol_utils::crc16CcittFalse(crcData);
            break;
        case StreamCrcType::Crc32Ieee:
            expected = protocol_utils::crc32Ieee(crcData);
            break;
        case StreamCrcType::None:
            break;
        }

        const auto actual = readCrcValue(frameBytes, frameLength, crcWidth, frame.crc.order);
        if (expected != actual) {
            result.action = AnalyzeResult::Action::RecoverableError;
            result.error = StreamParseError{
                .code = StreamParseErrorCode::CrcMismatch,
                .message = "CRC 校验失败",
                .frameName = frame.name,
                .droppedBytes = 0,
                .raw = copyBytes(frameBytes, frameLength),
            };
            return result;
        }
    }

    StreamFieldMap parsedFields;
    parsedFields.reserve(frame.fields.size());
    std::size_t cursor = 0;
    const auto readableLimit = frameLength - crcWidth;
    const auto frameBytesVector = copyBytes(frameBytes, frameLength);
    for (const auto& field : frame.fields) {
        const auto width = streamValueWidth(field.type);
        const auto start = field.offset.value_or(cursor);
        std::string countError;
        const auto count = resolveFieldCount(field, parsedFields, frameLength, readableLimit, start, frameBytesVector, countError);
        if (!count.has_value()) {
            result.action = AnalyzeResult::Action::RecoverableError;
            result.error = StreamParseError{
                .code = StreamParseErrorCode::CountResolveFailed,
                .message = countError.empty() ? "字段数量解析失败" : countError,
                .frameName = frame.name,
                .droppedBytes = 0,
                .raw = copyBytes(frameBytes, frameLength),
            };
            return result;
        }

        if (start > readableLimit || (*count > 0 && start + (*count * width) > readableLimit)) {
            result.action = AnalyzeResult::Action::RecoverableError;
            result.error = StreamParseError{
                .code = StreamParseErrorCode::FieldDecodeFailed,
                .message = "字段超出帧有效载荷范围",
                .frameName = frame.name,
                .droppedBytes = 0,
                .raw = copyBytes(frameBytes, frameLength),
            };
            return result;
        }

        if (field.type == StreamValueType::Bytes) {
            parsedFields[field.name] = StreamFieldValue{copyBytes(frameBytes + start, *count)};
        } else if (streamValueTypeIsFloat(field.type)) {
            if (*count == 1) {
                parsedFields[field.name] = StreamFieldValue{decodeFloat(frameBytesVector, start, field.type).value_or(0.0)};
            } else {
                std::vector<double> values;
                values.reserve(*count);
                for (std::size_t index = 0; index < *count; ++index) {
                    values.push_back(decodeFloat(frameBytesVector, start + index * width, field.type).value_or(0.0));
                }
                parsedFields[field.name] = StreamFieldValue{std::move(values)};
            }
        } else {
            if (*count == 1) {
                parsedFields[field.name] = StreamFieldValue{decodeInteger(frameBytesVector, start, field.type).value_or(0)};
            } else {
                std::vector<std::int64_t> values;
                values.reserve(*count);
                for (std::size_t index = 0; index < *count; ++index) {
                    values.push_back(decodeInteger(frameBytesVector, start + index * width, field.type).value_or(0));
                }
                parsedFields[field.name] = StreamFieldValue{std::move(values)};
            }
        }

        cursor = std::max(cursor, start + (*count * width));
    }

    result.action = AnalyzeResult::Action::Parsed;
    result.frameLength = frameLength;
    result.frame = StreamParsedFrame{
        .name = frame.name,
        .raw = copyBytes(frameBytes, frameLength),
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
                                               std::string& error) const {
    if (!definition.runtimeProfile) {
        return true;
    }
    const auto profileIter = runtimeProfiles_.find(definition.name);
    if (profileIter == runtimeProfiles_.end()) {
        error = "runtime_profile 帧缺少 channel_map";
        return false;
    }
    frame.channelMap = profileIter->second.channelMap;
    if (frame.channelMap.empty()) {
        return true;
    }
    std::vector<bool> used(frame.channelMap.size(), false);
    for (const auto target : frame.channelMap) {
        if (target >= frame.channelMap.size()) {
            error = "runtime profile channel_map 超出通道范围";
            return false;
        }
        if (used[target]) {
            error = "runtime profile channel_map 存在重复目标";
            return false;
        }
        used[target] = true;
    }
    return true;
}

ByteRingBuffer::LinearReadView FrameStreamParser::ensureLinearWindow(std::size_t count) const {
    return buffer_.linearRead(0, count, linearScratch_);
}

void FrameStreamParser::buildCompiledFrames() {
    compiledFrames_.clear();
    compiledFrames_.reserve(frames_.size());
    maxHeaderLength_ = 0;
    for (auto& bucket : headerFirstByteIndex_) {
        bucket.clear();
    }

    for (std::size_t index = 0; index < frames_.size(); ++index) {
        const auto& frame = frames_[index];
        CompiledFrame compiled;
        compiled.index = index;
        compiled.hasHeader = !frame.header.empty();
        compiled.firstHeaderByte = compiled.hasHeader ? frame.header.front() : 0;
        compiled.minFrameLength = frame.header.size();
        if (frame.size.has_value()) {
            compiled.minFrameLength = *frame.size;
        } else if (frame.len.has_value()) {
            compiled.minFrameLength = std::max(compiled.minFrameLength,
                                               frame.len->offset + streamValueWidth(frame.len->type));
        }

        for (const auto& field : frame.fields) {
            if (field.count.fixed.has_value()) {
                compiled.fixedFieldBytes += (*field.count.fixed) * streamValueWidth(field.type);
            }
        }

        compiled.minFrameLength = std::max(compiled.minFrameLength,
                                           frame.header.size() + compiled.fixedFieldBytes);
        maxHeaderLength_ = std::max(maxHeaderLength_, frame.header.size());
        compiledFrames_.push_back(compiled);
        if (compiled.hasHeader) {
            headerFirstByteIndex_[compiled.firstHeaderByte].push_back(index);
        }
    }
}

std::optional<std::size_t> FrameStreamParser::resolveFieldCount(const StreamFieldDefinition& field,
                                                                const StreamFieldMap& parsed,
                                                                std::size_t frameLength,
                                                                std::size_t readableLimit,
                                                                std::size_t fieldStart,
                                                                const std::vector<std::uint8_t>& frameBytes,
                                                                std::string& error) const {
    (void)frameBytes;
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
        if (static_cast<std::uint64_t>(*value) > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
            error = "count 表达式结果过大";
            return std::nullopt;
        }
        return static_cast<std::size_t>(*value);
    }
    return 1U;
}

std::string_view streamParseErrorCodeName(StreamParseErrorCode code) {
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

std::size_t streamValueWidth(StreamValueType type) {
    switch (type) {
    case StreamValueType::U8:
    case StreamValueType::I8:
    case StreamValueType::Bytes:
        return 1;
    case StreamValueType::U16Be:
    case StreamValueType::U16Le:
    case StreamValueType::I16Be:
    case StreamValueType::I16Le:
        return 2;
    case StreamValueType::U32Be:
    case StreamValueType::U32Le:
    case StreamValueType::I32Be:
    case StreamValueType::I32Le:
    case StreamValueType::F32Be:
    case StreamValueType::F32Le:
        return 4;
    }
    return 1;
}

bool streamValueTypeIsFloat(StreamValueType type) {
    return type == StreamValueType::F32Be || type == StreamValueType::F32Le;
}

bool streamValueTypeIsSigned(StreamValueType type) {
    return type == StreamValueType::I8
        || type == StreamValueType::I16Be
        || type == StreamValueType::I16Le
        || type == StreamValueType::I32Be
        || type == StreamValueType::I32Le;
}

} // namespace protoscope::scripting
