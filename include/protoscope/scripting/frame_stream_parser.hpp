#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace protoscope::scripting {

class ByteRingBuffer {
public:
    struct LinearReadView {
        const std::uint8_t* data{nullptr};
        std::size_t size{0};
        bool copied{false};
    };

    explicit ByteRingBuffer(std::size_t capacity = 0);

    void reset();
    void ensureCapacity(std::size_t capacity);
    [[nodiscard]] std::size_t capacity() const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool empty() const;

    std::size_t append(const std::vector<std::uint8_t>& bytes, bool dropOldest);
    void discardFront(std::size_t count);
    [[nodiscard]] std::uint8_t at(std::size_t index) const;
    [[nodiscard]] std::vector<std::uint8_t> slice(std::size_t offset, std::size_t count) const;
    [[nodiscard]] LinearReadView linearRead(std::size_t offset,
                                            std::size_t count,
                                            std::vector<std::uint8_t>& scratch) const;

private:
    void appendContiguous(const std::uint8_t* bytes, std::size_t count);

    std::vector<std::uint8_t> storage_;
    std::size_t head_{0};
    std::size_t size_{0};
};

enum class StreamValueType {
    U8,
    I8,
    U16Be,
    U16Le,
    I16Be,
    I16Le,
    U32Be,
    U32Le,
    I32Be,
    I32Le,
    F32Be,
    F32Le,
    Bytes,
};

enum class StreamLengthMeans {
    Payload,
    Frame,
};

enum class StreamCrcType {
    None,
    Crc16Modbus,
    Crc16CcittFalse,
    Crc32Ieee,
};

enum class StreamCrcOrder {
    HiLo,
    LoHi,
};

struct StreamFieldValue;
using StreamFieldMap = std::unordered_map<std::string, StreamFieldValue>;

enum class StreamCountExpressionOp {
    Constant,
    Field,
    Div,
    Sub,
    Mul,
    Remaining,
    IfFlag,
    Case,
    BitCount,
};

struct StreamCountExpression;

struct StreamCountCase {
    std::int64_t value{0};
    std::shared_ptr<StreamCountExpression> expression;
};

struct StreamCountExpression {
    StreamCountExpressionOp op{StreamCountExpressionOp::Constant};
    std::int64_t value{0};
    std::int64_t argument{0};
    std::string fieldName;
    bool excludeCrc{true};
    std::shared_ptr<StreamCountExpression> operand;
    std::shared_ptr<StreamCountExpression> argumentExpression;
    std::shared_ptr<StreamCountExpression> thenExpression;
    std::shared_ptr<StreamCountExpression> elseExpression;
    std::shared_ptr<StreamCountExpression> defaultExpression;
    std::vector<StreamCountCase> cases;
};

struct StreamFieldCount {
    std::optional<std::size_t> fixed;
    std::optional<std::string> fieldName;
    std::shared_ptr<StreamCountExpression> expression;
};

struct StreamFieldValue {
    using Storage =
        std::variant<std::int64_t, double, std::vector<std::int64_t>, std::vector<double>, std::vector<std::uint8_t>>;

    Storage value;

    [[nodiscard]] bool isIntegerScalar() const;
    [[nodiscard]] std::optional<std::int64_t> integerScalar() const;
};

struct StreamFieldDefinition {
    std::string name;
    StreamValueType type{StreamValueType::U8};
    std::optional<std::size_t> offset;
    StreamFieldCount count;
};

struct StreamLengthDefinition {
    std::size_t offset{0};
    StreamValueType type{StreamValueType::U8};
    StreamLengthMeans means{StreamLengthMeans::Payload};
    std::size_t extra{0};
};

struct StreamCrcDefinition {
    StreamCrcType type{StreamCrcType::None};
    StreamCrcOrder order{StreamCrcOrder::LoHi};
};

struct StreamFrameDefinition {
    std::string name;
    std::vector<std::uint8_t> header;
    std::optional<std::size_t> size;
    std::optional<StreamLengthDefinition> len;
    StreamCrcDefinition crc;
    std::vector<StreamFieldDefinition> fields;
    bool runtimeProfile{false};
};

struct StreamBufferDefinition {
    std::size_t capacity{4096};
    std::size_t maxCapacity{256U * 1024U * 1024U};
    bool dropOldest{false};
    double nearOverflowThresholdRatio{0.8};
    bool nearOverflowNotify{true};
    bool popupEnabled{true};
};

struct StreamParsedFrame {
    std::string name;
    std::vector<std::uint8_t> raw;
    StreamFieldMap fields;
    bool crcOk{true};
    std::vector<std::size_t> channelMap;
};

struct StreamRuntimeProfile {
    std::size_t length{0};
    std::vector<std::size_t> channelMap;
};

enum class StreamParseErrorCode {
    Overflow,
    NoiseDiscarded,
    InvalidLength,
    CrcMismatch,
    FieldDecodeFailed,
    CountResolveFailed,
};

struct StreamParseError {
    StreamParseErrorCode code{StreamParseErrorCode::InvalidLength};
    std::string message;
    std::optional<std::string> frameName;
    std::size_t droppedBytes{0};
    std::vector<std::uint8_t> raw;
};

struct StreamParseBatch {
    std::vector<StreamParsedFrame> frames;
    std::vector<StreamParseError> errors;
    std::size_t bufferSize{0};
    std::size_t bufferCapacity{0};
    bool nearOverflow{false};
    bool overflowed{false};
    std::size_t droppedBytes{0};
};

class FrameStreamParser {
public:
    FrameStreamParser(StreamBufferDefinition buffer, std::vector<StreamFrameDefinition> frames);

    void reset();
    [[nodiscard]] const StreamBufferDefinition& bufferDefinition() const;
    [[nodiscard]] const std::vector<StreamFrameDefinition>& frameDefinitions() const;
    void clearRuntimeProfiles();
    bool setRuntimeProfile(const std::string& frameName, StreamRuntimeProfile profile, std::string& error);
    bool clearRuntimeProfile(const std::optional<std::string>& frameName, std::string& error);
    StreamParseBatch pushBytes(const std::vector<std::uint8_t>& bytes);

private:
    struct CompiledFrame {
        std::size_t index{0};
        std::size_t minFrameLength{0};
        std::size_t fixedFieldBytes{0};
        std::uint8_t firstHeaderByte{0};
        bool hasHeader{false};
    };

    struct CandidateMatch {
        std::size_t start{0};
        const std::vector<std::size_t>* indexes{nullptr};
    };

    struct AnalyzeResult {
        enum class Action {
            NeedMore,
            Parsed,
            RecoverableError,
        };

        Action action{Action::NeedMore};
        std::optional<StreamParsedFrame> frame;
        std::optional<StreamParseError> error;
        std::size_t frameLength{0};
    };

    [[nodiscard]] std::size_t maxHeaderLength() const;
    [[nodiscard]] std::optional<CandidateMatch> findCandidate() const;
    AnalyzeResult analyzeFrame(const CompiledFrame& compiled, const ByteRingBuffer::LinearReadView& window) const;
    [[nodiscard]] bool applyRuntimeChannelMap(const StreamFrameDefinition& definition,
                                              StreamParsedFrame& frame,
                                              std::string& error) const;
    std::optional<std::size_t> resolveFieldCount(const StreamFieldDefinition& field,
                                                 const StreamFieldMap& parsed,
                                                 std::size_t frameLength,
                                                 std::size_t readableLimit,
                                                 std::size_t fieldStart,
                                                 std::string& error) const;
    [[nodiscard]] ByteRingBuffer::LinearReadView ensureLinearWindow(std::size_t count) const;
    void buildCompiledFrames();

private:
    StreamBufferDefinition bufferDefinition_;
    std::vector<StreamFrameDefinition> frames_;
    ByteRingBuffer buffer_;
    std::vector<CompiledFrame> compiledFrames_;
    std::array<std::vector<std::size_t>, 256> headerFirstByteIndex_{};
    std::array<std::vector<std::size_t>, 256> sortedHeaderFirstByteIndex_{};
    std::size_t maxHeaderLength_{0};
    mutable std::vector<std::uint8_t> linearScratch_;
    mutable std::vector<std::size_t> matchedCandidateIndexes_;
    std::unordered_map<std::string, StreamRuntimeProfile> runtimeProfiles_;
};

std::string_view streamParseErrorCodeName(StreamParseErrorCode code);
std::size_t streamValueWidth(StreamValueType type);
bool streamValueTypeIsFloat(StreamValueType type);
bool streamValueTypeIsSigned(StreamValueType type);

} // namespace protoscope::scripting
