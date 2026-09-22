#include "protoscope/data/model.hpp"

#include <bit>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace protoscope::data {
namespace {

    [[noreturn]] void invalid(const char* message) { throw std::invalid_argument(message); }

    void checkName(const std::string& name)
    {
        if (name.empty() || name.size() > 128 || name.find('\0') != std::string::npos) {
            invalid("数据集或字段名称必须为 1 到 128 字节且不含 NUL");
        }
    }

    struct Encoder {
        Bytes result;
        ValueLimits limits;
        std::size_t nodes{0};

        void byte(std::uint8_t value)
        {
            if (result.size() >= limits.maxBytes) invalid("值超过字节上限");
            result.push_back(value);
        }
        void integer(std::uint64_t value)
        {
            for (int i = 0; i < 8; ++i) byte(static_cast<std::uint8_t>(value >> (8 * i)));
        }
        void bytes(std::span<const std::uint8_t> value)
        {
            integer(value.size());
            if (value.size() > limits.maxBytes - result.size()) invalid("值超过字节上限");
            result.insert(result.end(), value.begin(), value.end());
        }
        void text(const std::string& value)
        {
            bytes({reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
        }
        void encode(const Value& input, std::size_t depth)
        {
            if (nodes>=limits.maxNodes) invalid("值超过节点数量上限");
            ++nodes;
            if (depth > limits.maxDepth) invalid("值超过嵌套深度上限");
            byte(static_cast<std::uint8_t>(input.value.index()));
            switch (input.value.index()) {
            case 0: break;
            case 1: integer(std::bit_cast<std::uint64_t>(std::get<std::int64_t>(input.value))); break;
            case 2: {
                const auto value = std::get<double>(input.value);
                if (!std::isfinite(value)) invalid("不支持非有限浮点数");
                integer(std::bit_cast<std::uint64_t>(value));
                break;
            }
            case 3: byte(std::get<bool>(input.value) ? 1 : 0); break;
            case 4: text(std::get<std::string>(input.value)); break;
            case 5: bytes(std::get<Bytes>(input.value)); break;
            case 6: {
                const auto& values = std::get<Value::Array>(input.value);
                integer(values.size());
                for (const auto& value : values) encode(value, depth + 1);
                break;
            }
            case 7: {
                const auto& values = std::get<Value::Object>(input.value);
                integer(values.size());
                for (const auto& [key, value] : values) {
                    text(key);
                    encode(value, depth + 1);
                }
                break;
            }
            default: invalid("未知值类型");
            }
        }
    };

    struct Decoder {
        std::span<const std::uint8_t> remaining;
        ValueLimits limits;
        std::size_t nodes{0};

        std::uint8_t byte()
        {
            if (remaining.empty()) invalid("值数据被截断");
            const auto result = remaining.front();
            remaining = remaining.subspan(1);
            return result;
        }
        std::uint64_t integer()
        {
            std::uint64_t result = 0;
            for (int i = 0; i < 8; ++i) result |= static_cast<std::uint64_t>(byte()) << (8 * i);
            return result;
        }
        std::size_t count()
        {
            const auto result = integer();
            // 每个元素至少占一个标签，分配前检查剩余输入，拒绝伪造的大长度。
            if (result > remaining.size()) invalid("值长度超出输入范围");
            return static_cast<std::size_t>(result);
        }
        std::span<const std::uint8_t> bytes()
        {
            const auto size = count();
            const auto result = remaining.first(size);
            remaining = remaining.subspan(size);
            return result;
        }
        std::string text()
        {
            const auto value = bytes();
            return {reinterpret_cast<const char*>(value.data()), value.size()};
        }
        Value decode(std::size_t depth)
        {
            // 小标签也会分配 Value/容器，交换文件必须能限制解码放大。
            if (nodes>=limits.maxNodes) invalid("值超过节点数量上限");
            ++nodes;
            if (depth > limits.maxDepth) invalid("值超过嵌套深度上限");
            switch (byte()) {
            case 0: return {};
            case 1: return {std::bit_cast<std::int64_t>(integer())};
            case 2: {
                const auto value = std::bit_cast<double>(integer());
                if (!std::isfinite(value)) invalid("不支持非有限浮点数");
                return {value};
            }
            case 3: {
                const auto value = byte();
                if (value > 1) invalid("布尔值编码无效");
                return {value != 0};
            }
            case 4: return {text()};
            case 5: {
                const auto value = bytes();
                return {Bytes(value.begin(), value.end())};
            }
            case 6: {
                const auto size = count();
                Value::Array values;
                for (std::size_t i = 0; i < size; ++i) values.push_back(decode(depth + 1));
                return {std::move(values)};
            }
            case 7: {
                const auto size = count();
                Value::Object values;
                for (std::size_t i = 0; i < size; ++i) {
                    auto key = text();
                    auto value = decode(depth + 1);
                    if (!values.emplace(std::move(key), std::move(value)).second) invalid("对象键重复");
                }
                return {std::move(values)};
            }
            default: invalid("未知值类型");
            }
        }
    };

    template<class T>
    const T& as(const Value& value)
    {
        const auto* result = std::get_if<T>(&value.value);
        if (result == nullptr) invalid("结构化值类型不匹配");
        return *result;
    }

    const Value::Array& array(const Value& value, std::size_t size)
    {
        const auto& result = as<Value::Array>(value);
        if (result.size() != size) invalid("结构化值字段数量不匹配");
        return result;
    }
}

Bytes encodeValue(const Value& value, ValueLimits limits)
{
    Encoder encoder{{}, limits};
    encoder.encode(value, 0);
    return std::move(encoder.result);
}

Value decodeValue(std::span<const std::uint8_t> bytes, ValueLimits limits)
{
    if (bytes.size() > limits.maxBytes) invalid("值超过字节上限");
    Decoder decoder{bytes, limits};
    auto value = decoder.decode(0);
    if (!decoder.remaining.empty()) invalid("值数据包含多余尾部");
    return value;
}

void validateSchema(const Schema& schema)
{
    checkName(schema.dataset);
    if (schema.fields.empty() || schema.fields.size() > 1024) invalid("字段数量必须为 1 到 1024");
    std::set<std::string> names;
    for (const auto& field : schema.fields) {
        checkName(field.name);
        if (!names.insert(field.name).second) invalid("数据集字段名称重复");
        if (field.type < FieldType::Int64 || field.type > FieldType::Bytes) invalid("不支持的字段类型");
    }
}

void validateRecord(const Schema& schema, const Record& record)
{
    validateSchema(schema);
    if (record.protocol.empty() || record.protocol.size() > 4096 || record.device.size() > 4096) {
        invalid("协议或设备标识无效");
    }
    if (record.dataset != schema.dataset || record.values.size() != schema.fields.size()) {
        invalid("记录与数据集模式不匹配");
    }
    for (std::size_t i = 0; i < record.values.size(); ++i) {
        const auto& value = record.values[i].value;
        const auto& field = schema.fields[i];
        if (value.index() == 0) {
            if (!field.nullable) invalid("必填字段不能为 null");
        } else if (value.index() != static_cast<std::size_t>(field.type)) {
            invalid("记录字段类型不匹配");
        }
        if (const auto* number = std::get_if<double>(&value); number && !std::isfinite(*number)) {
            invalid("记录不能包含非有限浮点数");
        }
    }
}

Value schemaValue(const Schema& schema)
{
    validateSchema(schema);
    Value::Array fields;
    for (const auto& field : schema.fields) {
        fields.push_back({Value::Array{{field.name}, {static_cast<std::int64_t>(field.type)}, {field.nullable}}});
    }
    return {Value::Array{{schema.dataset}, {std::move(fields)}}};
}

Schema schemaFromValue(const Value& value)
{
    const auto& parts = array(value, 2);
    Schema schema{as<std::string>(parts[0]), {}};
    for (const auto& encoded : as<Value::Array>(parts[1])) {
        const auto& field = array(encoded, 3);
        const auto type = as<std::int64_t>(field[1]);
        if (type < 1 || type > 5) invalid("不支持的字段类型");
        schema.fields.push_back({as<std::string>(field[0]), static_cast<FieldType>(type), as<bool>(field[2])});
    }
    validateSchema(schema);
    return schema;
}

Value recordValue(const Record& record)
{
    if (record.schemaVersion > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        invalid("模式版本超出范围");
    }
    return {Value::Array{{record.protocol}, {record.dataset}, {record.device}, {record.receivedAtUs},
        record.deviceTimeUs ? Value{*record.deviceTimeUs} : Value{},
        {static_cast<std::int64_t>(record.schemaVersion)}, {record.values}}};
}

Record recordFromValue(const Value& value)
{
    const auto& parts = array(value, 7);
    const auto version = as<std::int64_t>(parts[5]);
    if (version < 0) invalid("模式版本不能为负");
    Record record{as<std::string>(parts[0]), as<std::string>(parts[1]), as<std::string>(parts[2]),
                  as<std::int64_t>(parts[3]), {}, static_cast<std::uint64_t>(version),
                  as<Value::Array>(parts[6])};
    if (parts[4].value.index() != 0) record.deviceTimeUs = as<std::int64_t>(parts[4]);
    return record;
}

} // namespace protoscope::data
