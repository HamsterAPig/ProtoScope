#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace protoscope::data {

using Bytes = std::vector<std::uint8_t>;

struct Value {
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value>;
    // 前六个标签是数据集标量，数组和对象仅用于结构化值存储。
    std::variant<std::monostate, std::int64_t, double, bool, std::string, Bytes, Array, Object> value;
    bool operator==(const Value&) const = default;
};

enum class FieldType : std::uint8_t { Int64 = 1, Double, Bool, String, Bytes };

struct Field {
    std::string name;
    FieldType type{FieldType::Double};
    bool nullable{true};
    bool operator==(const Field&) const = default;
};

struct Schema {
    std::string dataset;
    std::vector<Field> fields;
    bool operator==(const Schema&) const = default;
};

struct Record {
    std::string protocol;
    std::string dataset;
    std::string device;
    std::int64_t receivedAtUs{0};
    std::optional<std::int64_t> deviceTimeUs;
    std::uint64_t schemaVersion{0};
    std::vector<Value> values;
    bool operator==(const Record&) const = default;
};

struct ValueLimits {
    std::size_t maxBytes{256U * 1024U};
    std::size_t maxDepth{16};
};

// 编解码严格保留整数、浮点位型、空值与字节；非法数据抛出 std::invalid_argument。
Bytes encodeValue(const Value& value, ValueLimits limits = {});
Value decodeValue(std::span<const std::uint8_t> bytes, ValueLimits limits = {});
void validateSchema(const Schema& schema);
void validateRecord(const Schema& schema, const Record& record);
Value schemaValue(const Schema& schema);
Schema schemaFromValue(const Value& value);
Value recordValue(const Record& record);
Record recordFromValue(const Value& value);

} // namespace protoscope::data
