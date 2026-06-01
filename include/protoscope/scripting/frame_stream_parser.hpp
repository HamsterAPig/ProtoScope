#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace protoscope::scripting {

class ByteRingBuffer {
public:
    explicit ByteRingBuffer(std::size_t capacity = 0);

    void reset();
    [[nodiscard]] std::size_t capacity() const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool empty() const;

    std::size_t append(const std::vector<std::uint8_t>& bytes, bool dropOldest);
    void discardFront(std::size_t count);
    [[nodiscard]] std::uint8_t at(std::size_t index) const;
    [[nodiscard]] std::vector<std::uint8_t> slice(std::size_t offset, std::size_t count) const;

private:
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
    using Storage = std::variant<std::int64_t,
                                 double,
                                 std::vector<std::int64_t>,
                                 std::vector<double>,
                                 std::vector<std::uint8_t>>;

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
};

struct StreamBufferDefinition {
    std::size_t capacity{4096};
    bool dropOldest{true};
};

struct StreamParsedFrame {
    std::string name;
    std::vector<std::uint8_t> raw;
    StreamFieldMap fields;
    bool crcOk{true};
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
};

class FrameStreamParser {
public:
    FrameStreamParser(StreamBufferDefinition buffer, std::vector<StreamFrameDefinition> frames);

    void reset();
    [[nodiscard]] const StreamBufferDefinition& bufferDefinition() const;
    [[nodiscard]] const std::vector<StreamFrameDefinition>& frameDefinitions() const;
    StreamParseBatch pushBytes(const std::vector<std::uint8_t>& bytes);

private:
    struct CandidateMatch {
        std::size_t start{0};
        std::vector<std::size_t> indexes;
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
    AnalyzeResult analyzeFrame(const StreamFrameDefinition& frame) const;
    std::optional<std::size_t> resolveFieldCount(const StreamFieldDefinition& field,
                                                 const StreamFieldMap& parsed,
                                                 std::size_t frameLength,
                                                 std::size_t readableLimit,
                                                 std::size_t fieldStart,
                                                 const std::vector<std::uint8_t>& frameBytes,
                                                 std::string& error) const;

private:
    StreamBufferDefinition bufferDefinition_;
    std::vector<StreamFrameDefinition> frames_;
    ByteRingBuffer buffer_;
};

std::string_view streamParseErrorCodeName(StreamParseErrorCode code);
std::size_t streamValueWidth(StreamValueType type);
bool streamValueTypeIsFloat(StreamValueType type);
bool streamValueTypeIsSigned(StreamValueType type);

} // namespace protoscope::scripting
