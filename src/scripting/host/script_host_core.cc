#include "protoscope/scripting/script_host.hpp"

#include "script_host_api_module.hpp"
#include "script_host_internal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <sol/sol.hpp>

namespace protoscope::scripting {

std::size_t ProtoBuffer::size() const
{
    return bytes.size();
}

ProtoBuffer ProtoBuffer::slice(std::size_t offset, std::size_t size) const
{
    if (offset >= bytes.size()) {
        return {};
    }
    const auto end = std::min(bytes.size(), offset + size);
    return ProtoBuffer{std::vector<std::uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                                 bytes.begin() + static_cast<std::ptrdiff_t>(end))};
}

std::string ProtoBuffer::toHex(std::size_t maxBytes) const
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    const auto limit = maxBytes == 0 ? bytes.size() : std::min(bytes.size(), maxBytes);
    std::string text;
    text.reserve(limit * 3);
    for (std::size_t index = 0; index < limit; ++index) {
        if (!text.empty()) {
            text.push_back(' ');
        }
        const auto value = bytes[index];
        text.push_back(kHex[(value >> 4U) & 0x0FU]);
        text.push_back(kHex[value & 0x0FU]);
    }
    return text;
}

constexpr const char* kDefaultDockId = "protocol";
constexpr const char* kDefaultDockTitle = "协议动作";

std::string readStringField(const sol::table& table, const char* key, std::string fallback = {})
{
    // 避免 sol2 字符串字段读取在 GCC 15 内联后触发数组边界误报。
    const sol::object value = table[key];
    if (!value.valid() || value.get_type() == sol::type::lua_nil || !value.is<std::string>()) {
        return fallback;
    }
    return value.as<std::string>();
}

std::uint64_t nowMs()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

double elapsedMilliseconds(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - start).count();
}

StreamParseBatch makeStreamParseBatchSnapshot(const StreamParseBatch& batch, bool lowOverhead)
{
    if (!lowOverhead) {
        return batch;
    }

    StreamParseBatch snapshot;
    snapshot.errors = batch.errors;
    snapshot.bufferSize = batch.bufferSize;
    snapshot.bufferCapacity = batch.bufferCapacity;
    snapshot.nearOverflow = batch.nearOverflow;
    snapshot.overflowed = batch.overflowed;
    snapshot.droppedBytes = batch.droppedBytes;
    // 核心流程：低开销模式的实时回调仍使用完整 batch，本地调试快照只保留摘要与错误，避免成功帧大 raw/字段重复常驻。
    return snapshot;
}

std::string kindName(transport::TransportKind kind)
{
    return std::string(transport::transportKindId(kind));
}

std::string txKindName(TxRequestKind kind)
{
    switch (kind) {
        case TxRequestKind::Send:
            return "send";
        case TxRequestKind::Request:
            return "request";
    }
    return "send";
}

std::string txRequestApiName(TxRequestKind kind, bool guarded)
{
    if (guarded) {
        return "proto.request_guarded";
    }
    return kind == TxRequestKind::Request ? "proto.request" : "proto.send";
}

std::string txEventStateName(TxEventState state)
{
    switch (state) {
        case TxEventState::Sent:
            return "sent";
        case TxEventState::Completed:
            return "completed";
        case TxEventState::Failed:
            return "failed";
        case TxEventState::Timeout:
            return "timeout";
        case TxEventState::Rejected:
            return "rejected";
        case TxEventState::Dropped:
            return "dropped";
        case TxEventState::Canceled:
            return "canceled";
    }
    return "rejected";
}

std::string dialogKindName(DialogKind kind)
{
    switch (kind) {
        case DialogKind::Alert:
            return "alert";
        case DialogKind::Confirm:
            return "confirm";
    }
    return "alert";
}

std::string fileDialogKindName(FileDialogKind kind)
{
    switch (kind) {
        case FileDialogKind::OpenFile:
            return "open_file";
        case FileDialogKind::SaveFile:
            return "save_file";
        case FileDialogKind::OpenDir:
            return "open_dir";
    }
    return "open_file";
}

std::optional<std::array<float, 4>> parseColorText(std::string_view text)
{
    if (text.size() != 7 && text.size() != 9) {
        return std::nullopt;
    }
    if (text.front() != '#') {
        return std::nullopt;
    }

    auto parseComponent = [&](std::size_t offset) -> std::optional<float> {
        const std::string component{text.substr(offset, 2)};
        char* end = nullptr;
        const auto value = std::strtoul(component.c_str(), &end, 16);
        if (end == nullptr || *end != '\0' || value > 255UL) {
            return std::nullopt;
        }
        return static_cast<float>(value) / 255.0F;
    };

    const auto red = parseComponent(1);
    const auto green = parseComponent(3);
    const auto blue = parseComponent(5);
    const auto alpha = text.size() == 9 ? parseComponent(7) : std::optional<float>{1.0F};
    if (!red.has_value() || !green.has_value() || !blue.has_value() || !alpha.has_value()) {
        return std::nullopt;
    }
    return std::array<float, 4>{*red, *green, *blue, *alpha};
}

ValueTableValue defaultValueTableFor(const ControlDescriptor& descriptor);
TxSequenceValue defaultTxSequenceFor(const ControlDescriptor& descriptor);

ControlValue defaultValueFor(const ControlDescriptor& descriptor)
{
    switch (descriptor.type) {
        case ControlType::Button:
            return false;
        case ControlType::InputText:
            return descriptor.textDefault;
        case ControlType::InputInt:
            return descriptor.intDefault;
        case ControlType::InputFloat:
            return descriptor.floatDefault;
        case ControlType::Checkbox:
            return descriptor.boolDefault;
        case ControlType::Combo:
            return descriptor.comboDefaultIndex;
        case ControlType::ElfSymbolCombo:
            return ElfSymbolValue{};
        case ControlType::ValueTable:
            return defaultValueTableFor(descriptor);
        case ControlType::TxSequence:
            return defaultTxSequenceFor(descriptor);
    }
    return false;
}

double finiteOrDefault(double value, double fallback)
{
    return std::isfinite(value) ? value : fallback;
}

std::string luaTypeName(sol::type type)
{
    switch (type) {
        case sol::type::lua_nil:
            return "nil";
        case sol::type::string:
            return "string";
        case sol::type::number:
            return "number";
        case sol::type::boolean:
            return "boolean";
        case sol::type::table:
            return "table";
        case sol::type::function:
            return "function";
        default:
            return "unknown";
    }
}

std::string serializeLuaObject(const sol::object& object, int depth)
{
    if (!object.valid() || object.get_type() == sol::type::lua_nil) {
        return "nil";
    }

    switch (object.get_type()) {
        case sol::type::string:
            return object.as<std::string>();
        case sol::type::boolean:
            return object.as<bool>() ? "true" : "false";
        case sol::type::number: {
            std::ostringstream builder;
            builder << object.as<double>();
            return builder.str();
        }
        case sol::type::table: {
            if (depth >= 3) {
                return "{...}";
            }
            std::ostringstream builder;
            builder << "{";
            bool first = true;
            for (const auto& pair : object.as<sol::table>()) {
                if (!first) {
                    builder << ", ";
                }
                first = false;
                builder << serializeLuaObject(pair.first, depth + 1) << "="
                        << serializeLuaObject(pair.second, depth + 1);
            }
            builder << "}";
            return builder.str();
        }
        default:
            return "<" + luaTypeName(object.get_type()) + ">";
    }
}

std::optional<ControlType> parseControlType(std::string_view value)
{
    if (value == "btn") {
        value = "button";
    } else if (value == "text") {
        value = "input_text";
    } else if (value == "int") {
        value = "input_int";
    } else if (value == "float") {
        value = "input_float";
    } else if (value == "check") {
        value = "checkbox";
    } else if (value == "select") {
        value = "combo";
    } else if (value == "symbol") {
        value = "elf_symbol_combo";
    } else if (value == "values") {
        value = "value_table";
    }

    if (value == "button") {
        return ControlType::Button;
    }
    if (value == "input_text") {
        return ControlType::InputText;
    }
    if (value == "input_int") {
        return ControlType::InputInt;
    }
    if (value == "input_float") {
        return ControlType::InputFloat;
    }
    if (value == "checkbox") {
        return ControlType::Checkbox;
    }
    if (value == "combo") {
        return ControlType::Combo;
    }
    if (value == "elf_symbol_combo") {
        return ControlType::ElfSymbolCombo;
    }
    if (value == "value_table") {
        return ControlType::ValueTable;
    }
    if (value == "tx_sequence") {
        return ControlType::TxSequence;
    }
    return std::nullopt;
}

std::string readStringFieldOrPosition(const sol::table& table,
                                      const char* key,
                                      std::size_t index,
                                      std::string fallback = {})
{
    const sol::object namedValue = table[key];
    if (namedValue.valid() && namedValue.get_type() != sol::type::lua_nil && namedValue.is<std::string>()) {
        return namedValue.as<std::string>();
    }
    const sol::object positionalValue = table[index];
    if (!positionalValue.valid() || positionalValue.get_type() == sol::type::lua_nil ||
        !positionalValue.is<std::string>()) {
        return fallback;
    }
    return positionalValue.as<std::string>();
}

std::optional<ControlLabelPosition> parseControlLabelPosition(std::string_view value)
{
    if (value == "left") {
        return ControlLabelPosition::Left;
    }
    if (value == "right") {
        return ControlLabelPosition::Right;
    }
    return std::nullopt;
}

bool controlAllowsEmptyLabel(ControlType type)
{
    return type == ControlType::Checkbox || type == ControlType::InputText || type == ControlType::InputInt ||
           type == ControlType::InputFloat;
}

ValueTableValue defaultValueTableFor(const ControlDescriptor& descriptor)
{
    ValueTableValue value;
    value.rows.resize(descriptor.valueRows.size());
    return value;
}

std::int64_t clampTxSequenceInteger(const TxSequenceFieldDescriptor& field, std::int64_t value)
{
    switch (field.type) {
        case TxSequenceFieldType::U8:
            return std::clamp<std::int64_t>(value, 0, 0xFF);
        case TxSequenceFieldType::U16:
            return std::clamp<std::int64_t>(value, 0, 0xFFFF);
        case TxSequenceFieldType::I16:
            return std::clamp<std::int64_t>(value, -32768, 32767);
        case TxSequenceFieldType::U32:
            return std::clamp<std::int64_t>(value, 0, static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()));
        case TxSequenceFieldType::String:
            break;
    }
    return value;
}

std::optional<std::int64_t> readLuaI64(const sol::object& object, std::string_view fieldName, std::string& error)
{
    if (!object.valid() || object.get_type() == sol::type::lua_nil) {
        error = std::string(fieldName) + " 必须是整数";
        return std::nullopt;
    }
    if (object.is<int>() || object.is<lua_Integer>()) {
        return static_cast<std::int64_t>(object.as<lua_Integer>());
    }
    if (object.is<double>()) {
        const auto number = object.as<double>();
        if (std::isfinite(number) && std::floor(number) == number &&
            number >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
            number <= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            return static_cast<std::int64_t>(number);
        }
    }
    error = std::string(fieldName) + " 必须是整数";
    return std::nullopt;
}

std::optional<TxSequenceFieldType> parseTxSequenceFieldType(std::string_view value)
{
    if (value == "u8") {
        return TxSequenceFieldType::U8;
    }
    if (value == "u16") {
        return TxSequenceFieldType::U16;
    }
    if (value == "i16") {
        return TxSequenceFieldType::I16;
    }
    if (value == "u32") {
        return TxSequenceFieldType::U32;
    }
    if (value == "string") {
        return TxSequenceFieldType::String;
    }
    return std::nullopt;
}

std::optional<TxSequenceFieldRadix> parseTxSequenceFieldRadix(std::string_view value)
{
    if (value.empty() || value == "dec") {
        return TxSequenceFieldRadix::Dec;
    }
    if (value == "hex") {
        return TxSequenceFieldRadix::Hex;
    }
    return std::nullopt;
}

TxSequenceFieldValue defaultTxSequenceFieldValue(const TxSequenceFieldDescriptor& field)
{
    if (field.type == TxSequenceFieldType::String) {
        if (const auto* text = std::get_if<std::string>(&field.defaultValue)) {
            return *text;
        }
        return std::string();
    }
    if (const auto* number = std::get_if<std::int64_t>(&field.defaultValue)) {
        return clampTxSequenceInteger(field, *number);
    }
    return std::int64_t{0};
}

TxSequenceFrameValue makeDefaultTxSequenceFrame(const ControlDescriptor& descriptor, std::uint32_t id)
{
    TxSequenceFrameValue frame;
    frame.id = id;
    frame.enabled = true;
    frame.name = "Frame " + std::to_string(id);
    for (const auto& field : descriptor.txSequenceFields) {
        frame.fields[field.id] = defaultTxSequenceFieldValue(field);
    }
    return frame;
}

TxSequenceValue defaultTxSequenceFor(const ControlDescriptor& descriptor)
{
    auto value = descriptor.txSequenceDefault;
    value.intervalMs = descriptor.txSequenceIntervalMs;
    value.loop = descriptor.txSequenceLoop;
    value.running = false;
    return value;
}

std::uint32_t nextTxSequenceFrameId(const TxSequenceValue& value)
{
    std::uint32_t maxId = 0;
    for (const auto& frame : value.frames) {
        maxId = std::max(maxId, frame.id);
    }
    return maxId == std::numeric_limits<std::uint32_t>::max() ? maxId : maxId + 1U;
}

struct ValueTableBitSource {
    std::optional<std::uint64_t> integer;
    std::vector<std::uint8_t> bytes;

    [[nodiscard]] bool bit(std::uint32_t index) const
    {
        if (integer.has_value()) {
            return index < 64U && (((*integer >> index) & 0x1ULL) != 0ULL);
        }

        const auto byteIndex = static_cast<std::size_t>(index / 8U);
        if (byteIndex >= bytes.size()) {
            return false;
        }
        const auto bitIndex = index % 8U;
        return ((bytes[byteIndex] >> bitIndex) & 0x1U) != 0U;
    }
};

ValueTableBitSource valueTableBitSourceFromInteger(std::uint64_t value)
{
    ValueTableBitSource source;
    source.integer = value;
    return source;
}

ValueTableBitSource valueTableBitSourceFromBytes(std::vector<std::uint8_t> bytes)
{
    ValueTableBitSource source;
    source.bytes = std::move(bytes);
    return source;
}

std::optional<std::uint32_t> readLuaU32(const sol::object& object, std::string_view fieldName, std::string& error)
{
    if (!object.valid() || object.get_type() == sol::type::lua_nil) {
        error = std::string(fieldName) + " 必须是整数";
        return std::nullopt;
    }
    if (!object.is<int>() && !object.is<lua_Integer>() && !object.is<double>()) {
        error = std::string(fieldName) + " 必须是整数";
        return std::nullopt;
    }

    if (object.is<int>() || object.is<lua_Integer>()) {
        const auto integer = object.as<lua_Integer>();
        if (integer < 0 || integer > static_cast<lua_Integer>(std::numeric_limits<std::uint32_t>::max())) {
            error = std::string(fieldName) + " 必须是 0..4294967295 范围内的整数";
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(integer);
    }

    const auto number = object.as<double>();
    if (!std::isfinite(number) || number < 0.0 ||
        number > static_cast<double>(std::numeric_limits<std::uint32_t>::max()) || std::floor(number) != number) {
        error = std::string(fieldName) + " 必须是 0..4294967295 范围内的整数";
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(number);
}

std::optional<std::uint32_t> readLuaBitIndex(const sol::object& object, std::string& error)
{
    return readLuaU32(object, "value_table bit", error);
}

std::string_view trimAsciiWhitespace(std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

std::optional<std::uint64_t> parseUnsignedIntegerString(std::string_view value, bool& integerLike)
{
    value = trimAsciiWhitespace(value);
    integerLike = false;
    if (value.empty()) {
        return std::nullopt;
    }

    int base = 10;
    if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        integerLike = true;
        value.remove_prefix(2);
        base = 16;
        if (value.empty()) {
            return std::nullopt;
        }
    } else {
        for (const char ch : value) {
            if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
                return std::nullopt;
            }
        }
        integerLike = true;
    }

    std::uint64_t parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, parsed, base);
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<ValueTableBitSource> readLuaBitSourceValue(const sol::object& object, std::string& error)
{
    if (!object.valid() || object.get_type() == sol::type::lua_nil) {
        error = "value_table bit 源值不能为空";
        return std::nullopt;
    }

    if (object.is<std::string>()) {
        const auto text = object.as<std::string>();
        bool integerLike = false;
        const auto integer = parseUnsignedIntegerString(text, integerLike);
        if (integerLike) {
            if (!integer.has_value()) {
                error = "value_table bit 源值字符串必须是非负整数";
                return std::nullopt;
            }
            return valueTableBitSourceFromInteger(*integer);
        }

        auto bytes = bytesFromLuaObject(object, error);
        if (!bytes.has_value()) {
            return std::nullopt;
        }
        return valueTableBitSourceFromBytes(std::move(*bytes));
    }

    if (object.is<int>() || object.is<lua_Integer>()) {
        const auto integer = object.as<lua_Integer>();
        if (integer < 0) {
            error = "value_table bit 源值必须是非负整数";
            return std::nullopt;
        }
        return valueTableBitSourceFromInteger(static_cast<std::uint64_t>(integer));
    }

    if (object.is<double>()) {
        const auto number = object.as<double>();
        if (!std::isfinite(number) || number < 0.0 ||
            number > static_cast<double>(std::numeric_limits<std::uint64_t>::max()) || std::floor(number) != number) {
            error = "value_table bit 源值必须是非负整数";
            return std::nullopt;
        }
        return valueTableBitSourceFromInteger(static_cast<std::uint64_t>(number));
    }

    auto bytes = bytesFromLuaObject(object, error);
    if (bytes.has_value()) {
        return valueTableBitSourceFromBytes(std::move(*bytes));
    }

    if (error.empty()) {
        error = "value_table bit 源值仅支持非负整数、整数字符串、HEX 字符串、ProtoBuffer 或 number[]";
    }
    return std::nullopt;
}

std::string scalarLuaValueToDisplayString(const sol::object& object)
{
    if (!object.valid() || object.get_type() == sol::type::lua_nil) {
        return {};
    }
    if (object.is<std::string>()) {
        return object.as<std::string>();
    }
    if (object.is<bool>()) {
        return object.as<bool>() ? "true" : "false";
    }
    if (object.is<int>()) {
        return std::to_string(object.as<int>());
    }
    if (object.is<double>()) {
        std::ostringstream builder;
        builder << object.as<double>();
        return builder.str();
    }
    return serializeLuaObject(object, 0);
}

void ensureValueTableSize(const ControlDescriptor& descriptor, ValueTableValue& value)
{
    value.rows.resize(descriptor.valueRows.size());
}

void setValueTableCell(const ControlDescriptor& descriptor,
                       ValueTableValue& value,
                       const std::size_t rowIndex,
                       std::string text)
{
    if (rowIndex >= descriptor.valueRows.size()) {
        return;
    }
    ensureValueTableSize(descriptor, value);
    value.rows[rowIndex].value = std::move(text);
    value.rows[rowIndex].set = true;
}

void applyValueTableBitRows(const ControlDescriptor& descriptor,
                            ValueTableValue& value,
                            const std::uint32_t sourceId,
                            const ValueTableBitSource& sourceValue)
{
    const auto bitRows = descriptor.valueBitRowsBySourceId.find(sourceId);
    if (bitRows == descriptor.valueBitRowsBySourceId.end()) {
        return;
    }

    for (const auto rowIndex : bitRows->second) {
        if (rowIndex >= descriptor.valueRows.size()) {
            continue;
        }
        const auto& row = descriptor.valueRows[rowIndex];
        if (!row.bit.has_value()) {
            continue;
        }
        const auto bitValue = sourceValue.bit(*row.bit) ? 1 : 0;
        setValueTableCell(descriptor, value, rowIndex, bitValue == 0 ? row.bitValues.zero : row.bitValues.one);
    }
}

bool applyValueTableLuaRegisterValue(const ControlDescriptor& descriptor,
                                     ValueTableValue& value,
                                     const std::uint32_t id,
                                     const sol::object& object,
                                     std::string& error)
{
    const auto rowIter = descriptor.valueRowById.find(id);
    if (rowIter != descriptor.valueRowById.end()) {
        setValueTableCell(descriptor, value, rowIter->second, scalarLuaValueToDisplayString(object));
    }

    if (descriptor.valueBitRowsBySourceId.find(id) != descriptor.valueBitRowsBySourceId.end()) {
        const auto sourceValue = readLuaBitSourceValue(object, error);
        if (!sourceValue.has_value()) {
            return false;
        }
        applyValueTableBitRows(descriptor, value, id, *sourceValue);
    }
    return true;
}

std::optional<ValueTableValue> valueTableValueFromLuaPatch(const ControlDescriptor& descriptor,
                                                           const sol::object& object,
                                                           std::string& error)
{
    ValueTableValue patch = defaultValueTableFor(descriptor);
    if (!object.valid() || object.get_type() == sol::type::lua_nil) {
        return patch;
    }
    if (!object.is<sol::table>()) {
        error = "value_table 控件仅支持 table";
        return std::nullopt;
    }

    const auto table = object.as<sol::table>();
    const sol::object startIdObject = table["start_id"];
    const sol::object valuesObject = table["values"];
    if (startIdObject.valid() && startIdObject.get_type() != sol::type::lua_nil) {
        const auto startId = readLuaU32(startIdObject, "value_table start_id", error);
        if (!startId.has_value()) {
            return std::nullopt;
        }
        if (!valuesObject.valid() || !valuesObject.is<sol::table>()) {
            error = "value_table range 更新必须提供 values 数组";
            return std::nullopt;
        }
        const auto values = valuesObject.as<sol::table>();
        for (std::size_t index = 1; index <= values.size(); ++index) {
            if (*startId > std::numeric_limits<std::uint32_t>::max() - static_cast<std::uint32_t>(index - 1U)) {
                error = "value_table range 更新 id 超出 U32";
                return std::nullopt;
            }
            const auto id = *startId + static_cast<std::uint32_t>(index - 1U);
            if (!applyValueTableLuaRegisterValue(descriptor, patch, id, values[index], error)) {
                return std::nullopt;
            }
        }
        return patch;
    }

    for (const auto& pair : table) {
        const auto id = readLuaU32(pair.first, "value_table row id", error);
        if (!id.has_value()) {
            return std::nullopt;
        }
        if (!applyValueTableLuaRegisterValue(descriptor, patch, *id, pair.second, error)) {
            return std::nullopt;
        }
    }
    return patch;
}

void mergeValueTableValue(const ControlDescriptor& descriptor, ValueTableValue& target, const ValueTableValue& patch)
{
    ensureValueTableSize(descriptor, target);
    for (std::size_t index = 0; index < patch.rows.size() && index < target.rows.size(); ++index) {
        if (!patch.rows[index].set) {
            continue;
        }
        target.rows[index] = patch.rows[index];
    }
}

std::string streamIntegerToDisplayString(std::int64_t value)
{
    return std::to_string(value);
}

std::string streamDoubleToDisplayString(double value)
{
    std::ostringstream builder;
    builder << value;
    return builder.str();
}

bool applyValueTableIntegerRegisterValue(const ControlDescriptor& descriptor,
                                         ValueTableValue& value,
                                         const std::uint32_t id,
                                         const std::int64_t stored)
{
    const auto rowIter = descriptor.valueRowById.find(id);
    if (rowIter != descriptor.valueRowById.end()) {
        setValueTableCell(descriptor, value, rowIter->second, streamIntegerToDisplayString(stored));
    }
    if (descriptor.valueBitRowsBySourceId.find(id) == descriptor.valueBitRowsBySourceId.end()) {
        return true;
    }
    if (stored < 0 || stored > static_cast<std::int64_t>(std::numeric_limits<lua_Integer>::max())) {
        return false;
    }
    applyValueTableBitRows(descriptor, value, id, valueTableBitSourceFromInteger(static_cast<std::uint64_t>(stored)));
    return true;
}

bool applyValueTableDoubleRegisterValue(const ControlDescriptor& descriptor,
                                        ValueTableValue& value,
                                        const std::uint32_t id,
                                        const double stored)
{
    const auto rowIter = descriptor.valueRowById.find(id);
    if (rowIter != descriptor.valueRowById.end()) {
        setValueTableCell(descriptor, value, rowIter->second, streamDoubleToDisplayString(stored));
    }
    if (descriptor.valueBitRowsBySourceId.find(id) == descriptor.valueBitRowsBySourceId.end()) {
        return true;
    }
    if (!std::isfinite(stored) || stored < 0.0 ||
        stored > static_cast<double>(std::numeric_limits<lua_Integer>::max()) || std::floor(stored) != stored) {
        return false;
    }
    applyValueTableBitRows(descriptor, value, id, valueTableBitSourceFromInteger(static_cast<std::uint64_t>(stored)));
    return true;
}

bool applyValueTableBytesRegisterValue(const ControlDescriptor& descriptor,
                                       ValueTableValue& value,
                                       const std::uint32_t id,
                                       const std::vector<std::uint8_t>& stored)
{
    const auto rowIter = descriptor.valueRowById.find(id);
    if (rowIter != descriptor.valueRowById.end()) {
        setValueTableCell(descriptor, value, rowIter->second, protocol_utils::bytesToHex(stored));
    }
    if (descriptor.valueBitRowsBySourceId.find(id) == descriptor.valueBitRowsBySourceId.end()) {
        return rowIter != descriptor.valueRowById.end();
    }
    // stream 的 bytes 字段整体视为一个 bit 源：bit0 是第 1 个字节最低位，bit8 是第 2 个字节最低位。
    applyValueTableBitRows(descriptor, value, id, valueTableBitSourceFromBytes(stored));
    return true;
}

std::optional<std::uint32_t> streamFieldU32(const StreamFieldValue& field)
{
    const auto value = field.integerScalar();
    if (!value.has_value() || *value < 0 ||
        *value > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*value);
}

bool applyStreamValueTargetValues(const ControlDescriptor& descriptor,
                                  ValueTableValue& patch,
                                  const std::uint32_t startId,
                                  const StreamFieldValue& values)
{
    if (const auto* bytes = std::get_if<std::vector<std::uint8_t>>(&values.value); bytes != nullptr) {
        return applyValueTableBytesRegisterValue(descriptor, patch, startId, *bytes);
    }
    if (const auto* integers = std::get_if<std::vector<std::int64_t>>(&values.value); integers != nullptr) {
        for (std::size_t index = 0; index < integers->size(); ++index) {
            if (startId > std::numeric_limits<std::uint32_t>::max() - static_cast<std::uint32_t>(index)) {
                return false;
            }
            if (!applyValueTableIntegerRegisterValue(
                    descriptor, patch, startId + static_cast<std::uint32_t>(index), (*integers)[index])) {
                return false;
            }
        }
        return true;
    }
    if (const auto* doubles = std::get_if<std::vector<double>>(&values.value); doubles != nullptr) {
        for (std::size_t index = 0; index < doubles->size(); ++index) {
            if (startId > std::numeric_limits<std::uint32_t>::max() - static_cast<std::uint32_t>(index)) {
                return false;
            }
            if (!applyValueTableDoubleRegisterValue(
                    descriptor, patch, startId + static_cast<std::uint32_t>(index), (*doubles)[index])) {
                return false;
            }
        }
        return true;
    }
    if (const auto integer = values.integerScalar(); integer.has_value()) {
        return applyValueTableIntegerRegisterValue(descriptor, patch, startId, *integer);
    }
    return false;
}

bool isValidDockAnchor(std::string_view value)
{
    return value == "left" || value == "left_bottom" || value == "right_top" || value == "right_mid" ||
           value == "right_bottom" || value == "main_bottom";
}

std::optional<TxSequenceFieldValue> txSequenceFieldValueFromLua(const TxSequenceFieldDescriptor& field,
                                                                const sol::object& object,
                                                                std::string& error)
{
    if (!object.valid() || object.get_type() == sol::type::lua_nil) {
        return defaultTxSequenceFieldValue(field);
    }
    if (field.type == TxSequenceFieldType::String) {
        if (!object.is<std::string>()) {
            error = "tx_sequence 字段 " + field.id + " 必须是 string";
            return std::nullopt;
        }
        return object.as<std::string>();
    }

    const auto number = readLuaI64(object, "tx_sequence 字段 " + field.id, error);
    if (!number.has_value()) {
        return std::nullopt;
    }
    const auto clamped = clampTxSequenceInteger(field, *number);
    if (clamped != *number) {
        error = "tx_sequence 字段 " + field.id + " 超出类型范围";
        return std::nullopt;
    }
    return clamped;
}

std::optional<TxSequenceFieldValue> requiredTxSequenceFieldValueFromLua(const TxSequenceFieldDescriptor& field,
                                                                        const sol::object& object,
                                                                        std::string_view fieldName,
                                                                        std::string& error)
{
    if (!object.valid() || object.get_type() == sol::type::lua_nil) {
        error = std::string(fieldName) + " 必须提供 value";
        return std::nullopt;
    }
    return txSequenceFieldValueFromLua(field, object, error);
}

std::optional<TxSequenceFrameValue> txSequenceFrameFromLua(const ControlDescriptor& descriptor,
                                                           const sol::object& object,
                                                           std::uint32_t fallbackId,
                                                           std::string& error)
{
    if (!object.is<sol::table>()) {
        error = "tx_sequence frames 元素必须是 table";
        return std::nullopt;
    }
    const auto table = object.as<sol::table>();
    auto frame = makeDefaultTxSequenceFrame(descriptor, fallbackId);
    const sol::object idObject = table["id"];
    if (idObject.valid() && idObject.get_type() != sol::type::lua_nil) {
        const auto id = readLuaU32(idObject, "tx_sequence frame id", error);
        if (!id.has_value()) {
            return std::nullopt;
        }
        frame.id = *id;
    }
    frame.enabled = table.get_or("enabled", true);
    frame.name = readStringField(table, "name", frame.name);

    const sol::object fieldsObject = table["fields"];
    if (fieldsObject.valid() && fieldsObject.get_type() != sol::type::lua_nil) {
        if (!fieldsObject.is<sol::table>()) {
            error = "tx_sequence frame fields 必须是 table";
            return std::nullopt;
        }
        const auto fieldsTable = fieldsObject.as<sol::table>();
        for (const auto& field : descriptor.txSequenceFields) {
            const auto parsed = txSequenceFieldValueFromLua(field, fieldsTable[field.id], error);
            if (!parsed.has_value()) {
                return std::nullopt;
            }
            frame.fields[field.id] = *parsed;
        }
    }
    return frame;
}

std::optional<TxSequenceValue> txSequenceValueFromLua(const ControlDescriptor& descriptor,
                                                      const sol::object& object,
                                                      std::string& error)
{
    if (!object.valid() || object.get_type() == sol::type::lua_nil) {
        return defaultTxSequenceFor(descriptor);
    }
    if (!object.is<sol::table>()) {
        error = "tx_sequence 控件仅支持 table";
        return std::nullopt;
    }

    const auto table = object.as<sol::table>();
    auto value = defaultTxSequenceFor(descriptor);
    value.intervalMs = std::max(1, table.get_or("interval_ms", value.intervalMs));
    value.loop = table.get_or("loop", value.loop);
    value.running = table.get_or("running", value.running);

    const sol::object framesObject = table["frames"];
    if (!framesObject.valid() || framesObject.get_type() == sol::type::lua_nil) {
        return value;
    }
    if (!framesObject.is<sol::table>()) {
        error = "tx_sequence frames 必须是 table";
        return std::nullopt;
    }

    value.frames.clear();
    std::unordered_set<std::uint32_t> ids;
    const auto framesTable = framesObject.as<sol::table>();
    for (std::size_t index = 1; index <= framesTable.size(); ++index) {
        const auto frame = txSequenceFrameFromLua(descriptor, framesTable[index], static_cast<std::uint32_t>(index), error);
        if (!frame.has_value()) {
            return std::nullopt;
        }
        if (!ids.insert(frame->id).second) {
            error = "tx_sequence frame id 不能重复: " + std::to_string(frame->id);
            return std::nullopt;
        }
        value.frames.push_back(*frame);
    }
    return value;
}

std::optional<ControlValue> controlValueFromLua(const ControlDescriptor& descriptor,
                                                const sol::object& object,
                                                std::string& error)
{
    if (!object.valid() || object.get_type() == sol::type::lua_nil) {
        return defaultValueFor(descriptor);
    }

    switch (descriptor.type) {
        case ControlType::Button:
        case ControlType::Checkbox:
            if (object.is<bool>()) {
                return object.as<bool>();
            }
            if (object.is<int>()) {
                return object.as<int>() != 0;
            }
            if (object.is<double>()) {
                return object.as<double>() != 0.0;
            }
            break;
        case ControlType::InputText:
            if (object.is<std::string>()) {
                return object.as<std::string>();
            }
            break;
        case ControlType::InputInt:
            if (object.is<int>()) {
                return object.as<int>();
            }
            if (object.is<double>()) {
                return static_cast<int>(object.as<double>());
            }
            break;
        case ControlType::InputFloat:
            if (object.is<float>()) {
                return object.as<float>();
            }
            if (object.is<double>()) {
                return static_cast<float>(object.as<double>());
            }
            if (object.is<int>()) {
                return static_cast<float>(object.as<int>());
            }
            break;
        case ControlType::Combo: {
            int index = 0;
            if (object.is<int>()) {
                index = object.as<int>();
            } else if (object.is<double>()) {
                index = static_cast<int>(object.as<double>());
            } else {
                error = "combo 控件仅支持 number";
                return std::nullopt;
            }
            if (descriptor.comboOptions.empty()) {
                return 0;
            }
            return std::clamp(index, 0, static_cast<int>(descriptor.comboOptions.size()) - 1);
        }
        case ControlType::ElfSymbolCombo: {
            if (object.get_type() == sol::type::lua_nil) {
                return ElfSymbolValue{};
            }
            if (!object.is<sol::table>()) {
                error = "elf_symbol_combo 控件仅支持 table 或 nil";
                return std::nullopt;
            }

            const auto table = object.as<sol::table>();
            ElfSymbolValue symbol;
            symbol.label = readStringField(table, "label");
            symbol.value = readStringField(table, "value");
            symbol.type = readStringField(table, "type");
            if (symbol.label.empty() || symbol.value.empty() || symbol.type.empty()) {
                error = "elf_symbol_combo 控件值必须包含 label、value、type";
                return std::nullopt;
            }
            return symbol;
        }
        case ControlType::ValueTable:
            return valueTableValueFromLuaPatch(descriptor, object, error);
        case ControlType::TxSequence:
            return txSequenceValueFromLua(descriptor, object, error);
    }

    error = "控件 " + descriptor.id + " 类型不匹配，实际收到 " + luaTypeName(object.get_type());
    return std::nullopt;
}

bool applyControlLabelPosition(ControlDescriptor& descriptor, const sol::table& table, std::string& error)
{
    const sol::object labelPositionObject = table["label_position"];
    if (!labelPositionObject.valid() || labelPositionObject.get_type() == sol::type::lua_nil) {
        return true;
    }
    if (!labelPositionObject.is<std::string>()) {
        error = "控件 label_position 必须是字符串 'left' 或 'right'";
        return false;
    }
    const auto labelPosition = parseControlLabelPosition(labelPositionObject.as<std::string>());
    if (!labelPosition.has_value()) {
        error = "控件 label_position 仅支持 'left' 或 'right'";
        return false;
    }
    descriptor.labelPosition = *labelPosition;
    return true;
}

bool applyControlCompactLabelConfig(ControlDescriptor& descriptor, const sol::table& table, std::string& error)
{
    const sol::object shortLabelObject = table["short_label"];
    if (shortLabelObject.valid() && shortLabelObject.get_type() != sol::type::lua_nil) {
        if (!shortLabelObject.is<std::string>()) {
            error = "控件 short_label 必须是非空字符串";
            return false;
        }
        descriptor.shortLabel = shortLabelObject.as<std::string>();
        if (descriptor.shortLabel.empty()) {
            error = "控件 short_label 必须是非空字符串";
            return false;
        }
    }

    const sol::object compactBelowObject = table["compact_label_below"];
    if (!compactBelowObject.valid() || compactBelowObject.get_type() == sol::type::lua_nil) {
        return true;
    }
    if (!compactBelowObject.is<double>() && !compactBelowObject.is<int>()) {
        error = "控件 compact_label_below 必须是 number";
        return false;
    }
    const double number = compactBelowObject.is<double>() ? compactBelowObject.as<double>()
                                                          : static_cast<double>(compactBelowObject.as<int>());
    if (!std::isfinite(number) || number <= 0.0) {
        error = "控件 compact_label_below 必须是正数";
        return false;
    }
    descriptor.compactLabelBelow = static_cast<float>(number);
    return true;
}

bool validateControlIdentity(const ControlDescriptor& descriptor, std::string& error)
{
    if (descriptor.id.empty()) {
        error = "控件必须提供 id";
        return false;
    }
    if (descriptor.label.empty() && !controlAllowsEmptyLabel(descriptor.type)) {
        // 核心流程：只有可用控件自身形态表达含义的紧凑型控件允许隐藏可见 label。
        error = "控件必须提供 label";
        return false;
    }
    return true;
}

std::optional<std::vector<std::string>> parseComboOptions(const sol::table& table, std::string& error)
{
    const sol::object optionsObject = table["options"];
    if (!optionsObject.valid() || !optionsObject.is<sol::table>()) {
        error = "combo 控件必须提供 options";
        return std::nullopt;
    }

    std::vector<std::string> optionsList;
    const auto options = optionsObject.as<sol::table>();
    for (std::size_t index = 1; index <= options.size(); ++index) {
        const sol::object option = options[index];
        if (!option.is<std::string>()) {
            error = "combo options 必须全部是 string";
            return std::nullopt;
        }
        optionsList.push_back(option.as<std::string>());
    }

    if (optionsList.empty()) {
        error = "combo options 不能为空";
        return std::nullopt;
    }
    return optionsList;
}

bool applyComboControlConfig(ControlDescriptor& descriptor, const sol::table& table, std::string& error)
{
    auto options = parseComboOptions(table, error);
    if (!options.has_value()) {
        return false;
    }
    descriptor.comboOptions = std::move(*options);

    const int defaultIndex = table.get_or("default", 1);
    descriptor.comboDefaultIndex = std::clamp(defaultIndex, 1, static_cast<int>(descriptor.comboOptions.size())) - 1;
    return true;
}

bool applyElfSymbolComboConfig(ControlDescriptor& descriptor, const sol::table& table, std::string& error)
{
    const sol::object debounceObject = table["debounce_ms"];
    if (debounceObject.valid() && debounceObject.get_type() != sol::type::lua_nil) {
        descriptor.debounceMs = debounceObject.as<int>();
        descriptor.debounceMsConfigured = true;
    }
    const sol::object limitObject = table["limit"];
    int limit = static_cast<int>(descriptor.limit);
    if (limitObject.valid() && limitObject.get_type() != sol::type::lua_nil) {
        limit = limitObject.as<int>();
        descriptor.limitConfigured = true;
    }
    if (descriptor.debounceMs <= 0) {
        error = "elf_symbol_combo debounce_ms 必须大于 0";
        return false;
    }
    if (limit <= 0) {
        error = "elf_symbol_combo limit 必须大于 0";
        return false;
    }
    descriptor.limit = static_cast<std::size_t>(limit);
    return true;
}

bool appendValueTableRow(ControlDescriptor& descriptor, ValueTableRowDescriptor row, std::string& error)
{
    if (row.label.empty()) {
        error = "value_table 行必须提供 label";
        return false;
    }

    const auto rowIndex = descriptor.valueRows.size();
    if (row.bit.has_value()) {
        descriptor.valueBitRowsBySourceId[row.sourceId].push_back(rowIndex);
    } else {
        if (descriptor.valueRowById.find(row.id) != descriptor.valueRowById.end()) {
            error = "value_table row id 不能重复: " + std::to_string(row.id);
            return false;
        }
        descriptor.valueRowById.emplace(row.id, rowIndex);
    }
    descriptor.valueRows.push_back(std::move(row));
    return true;
}

std::vector<std::string> readValueTableStringArray(const sol::object& object, const char* fieldName, std::string& error)
{
    std::vector<std::string> values;
    if (!object.valid() || object.get_type() == sol::type::lua_nil) {
        return values;
    }
    if (!object.is<sol::table>()) {
        error = std::string("value_table ") + fieldName + " 必须是 string 数组";
        return {};
    }

    const auto table = object.as<sol::table>();
    values.reserve(table.size());
    for (std::size_t index = 1; index <= table.size(); ++index) {
        const sol::object value = table[index];
        if (!value.is<std::string>()) {
            error = std::string("value_table ") + fieldName + " 必须全部是 string";
            return {};
        }
        values.push_back(value.as<std::string>());
    }
    return values;
}

bool applyValueTableBitValueLabels(ValueTableRowDescriptor& row, const sol::object& valuesObject, std::string& error)
{
    if (!valuesObject.valid() || valuesObject.get_type() == sol::type::lua_nil) {
        return true;
    }
    if (!valuesObject.is<sol::table>()) {
        error = "value_table bit values 必须是 table";
        return false;
    }

    for (const auto& pair : valuesObject.as<sol::table>()) {
        const auto key = readLuaU32(pair.first, "value_table bit values key", error);
        if (!key.has_value()) {
            return false;
        }
        if (*key > 1U || !pair.second.is<std::string>()) {
            error = "value_table bit values 仅支持 [0]/[1] = string";
            return false;
        }
        if (*key == 0U) {
            row.bitValues.zero = pair.second.as<std::string>();
        } else {
            row.bitValues.one = pair.second.as<std::string>();
        }
    }
    return true;
}

bool applyValueTableRangeRows(ControlDescriptor& descriptor, const sol::table& rowTable, std::string& error)
{
    const auto startId = readLuaU32(rowTable["start_id"], "value_table start_id", error);
    if (!startId.has_value()) {
        return false;
    }
    const auto len = readLuaU32(rowTable["len"], "value_table len", error);
    if (!len.has_value() || *len == 0U) {
        error = "value_table range len 必须大于 0";
        return false;
    }
    if (*startId > std::numeric_limits<std::uint32_t>::max() - (*len - 1U)) {
        error = "value_table range id 超出 U32";
        return false;
    }

    auto labels = readValueTableStringArray(rowTable["labels"], "labels", error);
    if (!error.empty()) {
        return false;
    }
    auto units = readValueTableStringArray(rowTable["units"], "units", error);
    if (!error.empty()) {
        return false;
    }
    if (labels.size() < *len) {
        error = "value_table range labels 数量必须不少于 len";
        return false;
    }

    for (std::uint32_t offset = 0; offset < *len; ++offset) {
        ValueTableRowDescriptor row;
        row.id = *startId + offset;
        row.sourceId = row.id;
        row.label = labels[offset];
        if (offset < units.size()) {
            row.unit = units[offset];
        }
        if (!appendValueTableRow(descriptor, std::move(row), error)) {
            return false;
        }
    }
    return true;
}

bool applyValueTableBitRows(ControlDescriptor& descriptor,
                            const sol::table& rowTable,
                            const std::uint32_t sourceId,
                            std::string& error)
{
    const sol::object bitsObject = rowTable["bits"];
    if (!bitsObject.valid() || !bitsObject.is<sol::table>()) {
        error = "value_table bits 必须是 table";
        return false;
    }

    const auto bits = bitsObject.as<sol::table>();
    for (std::size_t index = 1; index <= bits.size(); ++index) {
        const sol::object bitObject = bits[index];
        if (!bitObject.is<sol::table>()) {
            error = "value_table bits 元素必须是 table";
            return false;
        }
        const auto bitTable = bitObject.as<sol::table>();
        ValueTableRowDescriptor row;
        row.id = sourceId;
        row.sourceId = sourceId;
        row.bit = readLuaBitIndex(bitTable["bit"], error);
        if (!row.bit.has_value()) {
            return false;
        }
        row.label = readStringField(bitTable, "label");
        row.unit = readStringField(bitTable, "unit");
        row.note = readStringField(bitTable, "note");
        if (!applyValueTableBitValueLabels(row, bitTable["values"], error)) {
            return false;
        }
        if (!appendValueTableRow(descriptor, std::move(row), error)) {
            return false;
        }
    }
    return true;
}

bool applyValueTableControlConfig(ControlDescriptor& descriptor, const sol::table& table, std::string& error)
{
    const sol::object rowsObject = table["rows"];
    if (!rowsObject.valid() || !rowsObject.is<sol::table>()) {
        error = "value_table 控件必须提供 rows";
        return false;
    }

    const auto rows = rowsObject.as<sol::table>();
    for (std::size_t index = 1; index <= rows.size(); ++index) {
        const sol::object rowObject = rows[index];
        if (!rowObject.is<sol::table>()) {
            error = "value_table rows 元素必须是 table";
            return false;
        }
        const auto rowTable = rowObject.as<sol::table>();
        const sol::object startIdObject = rowTable["start_id"];
        if (startIdObject.valid() && startIdObject.get_type() != sol::type::lua_nil) {
            if (!applyValueTableRangeRows(descriptor, rowTable, error)) {
                return false;
            }
            continue;
        }

        const sol::object bitsObject = rowTable["bits"];
        const sol::object idObject = rowTable["id"];
        std::optional<std::uint32_t> id;
        if (idObject.valid() && idObject.get_type() != sol::type::lua_nil) {
            id = readLuaU32(idObject, "value_table row id", error);
            if (!id.has_value()) {
                return false;
            }
        } else {
            // 只有普通 value_table 行支持位置参数；bit 行保持显式 id + bits 写法。
            if (bitsObject.valid() && bitsObject.get_type() != sol::type::lua_nil) {
                error = "value_table row id 必须是整数";
                return false;
            }
            id = readLuaU32(rowTable[1], "value_table row id", error);
            if (!id.has_value()) {
                return false;
            }
        }
        if (bitsObject.valid() && bitsObject.get_type() != sol::type::lua_nil) {
            if (!applyValueTableBitRows(descriptor, rowTable, *id, error)) {
                return false;
            }
            continue;
        }

        ValueTableRowDescriptor row;
        row.id = *id;
        row.sourceId = *id;
        row.label = readStringFieldOrPosition(rowTable, "label", 2);
        row.unit = readStringFieldOrPosition(rowTable, "unit", 3);
        row.note = readStringField(rowTable, "note");
        if (!appendValueTableRow(descriptor, std::move(row), error)) {
            return false;
        }
    }
    if (descriptor.valueRows.empty()) {
        error = "value_table rows 不能为空";
        return false;
    }
    return true;
}

bool appendTxSequenceField(ControlDescriptor& descriptor, TxSequenceFieldDescriptor field, std::string& error)
{
    if (field.id.empty()) {
        error = "tx_sequence fields 元素必须提供 id";
        return false;
    }
    if (field.label.empty()) {
        field.label = field.id;
    }
    const auto duplicate = std::find_if(descriptor.txSequenceFields.begin(),
                                        descriptor.txSequenceFields.end(),
                                        [&](const auto& item) { return item.id == field.id; });
    if (duplicate != descriptor.txSequenceFields.end()) {
        error = "tx_sequence field id 不能重复: " + field.id;
        return false;
    }
    descriptor.txSequenceFields.push_back(std::move(field));
    return true;
}

bool applyTxSequenceFieldDefault(TxSequenceFieldDescriptor& field,
                                 const sol::object& defaultObject,
                                 std::string& error)
{
    if (!defaultObject.valid() || defaultObject.get_type() == sol::type::lua_nil) {
        field.defaultValue = defaultTxSequenceFieldValue(field);
        return true;
    }
    const auto parsed = txSequenceFieldValueFromLua(field, defaultObject, error);
    if (!parsed.has_value()) {
        return false;
    }
    field.defaultValue = *parsed;
    return true;
}

bool applyTxSequenceFieldOptions(TxSequenceFieldDescriptor& field,
                                 const sol::object& optionsObject,
                                 std::string& error)
{
    if (!optionsObject.valid() || optionsObject.get_type() == sol::type::lua_nil) {
        return true;
    }
    if (!optionsObject.is<sol::table>()) {
        error = "tx_sequence field options 必须是数组";
        return false;
    }

    const auto optionsTable = optionsObject.as<sol::table>();
    const auto optionCount = optionsTable.size();
    std::vector<bool> seen(optionCount, false);
    std::size_t visited = 0;
    for (const auto& pair : optionsTable) {
        const auto index = readLuaU32(pair.first, "tx_sequence field option index", error);
        if (!index.has_value() || *index == 0U || *index > optionCount) {
            error = "tx_sequence field options 必须是数组";
            return false;
        }
        if (seen[*index - 1U]) {
            error = "tx_sequence field options 必须是数组";
            return false;
        }
        seen[*index - 1U] = true;
        ++visited;
    }
    if (visited != optionCount) {
        error = "tx_sequence field options 必须是数组";
        return false;
    }

    field.options.clear();
    for (std::size_t index = 1; index <= optionCount; ++index) {
        const sol::object optionObject = optionsTable[index];
        if (!optionObject.is<sol::table>()) {
            error = "tx_sequence field options 元素必须是 table";
            return false;
        }

        const auto optionTable = optionObject.as<sol::table>();
        TxSequenceFieldOption option;
        option.label = readStringField(optionTable, "label");
        if (option.label.empty()) {
            error = "tx_sequence field option label 不能为空";
            return false;
        }
        const auto parsed =
            requiredTxSequenceFieldValueFromLua(field, optionTable["value"], "tx_sequence field option", error);
        if (!parsed.has_value()) {
            return false;
        }
        option.value = *parsed;
        field.options.push_back(std::move(option));
    }
    return true;
}

bool applyTxSequenceFieldsConfig(ControlDescriptor& descriptor, const sol::table& table, std::string& error)
{
    const sol::object fieldsObject = table["fields"];
    if (!fieldsObject.valid() || !fieldsObject.is<sol::table>()) {
        error = "tx_sequence 控件必须提供 fields";
        return false;
    }

    const auto fields = fieldsObject.as<sol::table>();
    if (fields.size() == 0) {
        error = "tx_sequence fields 不能为空";
        return false;
    }
    for (std::size_t index = 1; index <= fields.size(); ++index) {
        const sol::object fieldObject = fields[index];
        if (!fieldObject.is<sol::table>()) {
            error = "tx_sequence fields 元素必须是 table";
            return false;
        }
        const auto fieldTable = fieldObject.as<sol::table>();
        TxSequenceFieldDescriptor field;
        field.id = readStringFieldOrPosition(fieldTable, "id", 1);
        field.label = readStringFieldOrPosition(fieldTable, "label", 2, field.id);
        const auto typeText = readStringField(fieldTable, "type", "u16");
        const auto fieldType = parseTxSequenceFieldType(typeText);
        if (!fieldType.has_value()) {
            error = "tx_sequence field type 仅支持 u8、u16、i16、u32、string";
            return false;
        }
        field.type = *fieldType;
        const auto radix = parseTxSequenceFieldRadix(readStringField(fieldTable, "radix", "dec"));
        if (!radix.has_value()) {
            error = "tx_sequence field radix 仅支持 hex 或 dec";
            return false;
        }
        field.radix = *radix;
        if (field.type == TxSequenceFieldType::String && field.radix == TxSequenceFieldRadix::Hex) {
            error = "tx_sequence string 字段不支持 radix = hex";
            return false;
        }
        if (!applyTxSequenceFieldDefault(field, fieldTable["default"], error)) {
            return false;
        }
        if (!applyTxSequenceFieldOptions(field, fieldTable["options"], error)) {
            return false;
        }
        if (!appendTxSequenceField(descriptor, std::move(field), error)) {
            return false;
        }
    }
    return true;
}

bool applyTxSequenceControlConfig(ControlDescriptor& descriptor, const sol::table& table, std::string& error)
{
    descriptor.txSequenceIntervalMs = std::max(1, table.get_or("interval_ms", 100));
    descriptor.txSequenceLoop = table.get_or("loop", false);
    if (!applyTxSequenceFieldsConfig(descriptor, table, error)) {
        return false;
    }

    descriptor.txSequenceDefault.intervalMs = descriptor.txSequenceIntervalMs;
    descriptor.txSequenceDefault.loop = descriptor.txSequenceLoop;
    descriptor.txSequenceDefault.running = false;
    descriptor.txSequenceDefault.frames.clear();

    const sol::object defaultObject = table["default"];
    if (defaultObject.valid() && defaultObject.get_type() != sol::type::lua_nil) {
        const auto parsed = txSequenceValueFromLua(descriptor, defaultObject, error);
        if (!parsed.has_value()) {
            return false;
        }
        descriptor.txSequenceDefault = *parsed;
        descriptor.txSequenceDefault.running = false;
    }
    return true;
}

bool applyControlTypeConfig(ControlDescriptor& descriptor, const sol::table& table, std::string& error)
{
    switch (descriptor.type) {
        case ControlType::Button:
            return true;
        case ControlType::InputText:
            descriptor.textDefault = readStringField(table, "default");
            return true;
        case ControlType::InputInt:
            descriptor.intDefault = table.get_or("default", 0);
            return true;
        case ControlType::InputFloat:
            descriptor.floatDefault = table.get_or("default", 0.0F);
            return true;
        case ControlType::Checkbox:
            descriptor.boolDefault = table.get_or("default", false);
            return true;
        case ControlType::Combo:
            return applyComboControlConfig(descriptor, table, error);
        case ControlType::ElfSymbolCombo:
            return applyElfSymbolComboConfig(descriptor, table, error);
        case ControlType::ValueTable:
            return applyValueTableControlConfig(descriptor, table, error);
        case ControlType::TxSequence:
            return applyTxSequenceControlConfig(descriptor, table, error);
    }
    return true;
}

sol::object txSequenceFieldValueToLua(sol::state_view lua, const TxSequenceFieldValue& value)
{
    return std::visit(
        [&lua](const auto& current) -> sol::object {
            return sol::make_object(lua, current);
        },
        value);
}

sol::object txSequenceValueToLua(sol::state_view lua,
                                 const ControlDescriptor& descriptor,
                                 const TxSequenceValue& value)
{
    sol::table table = lua.create_table();
    table["interval_ms"] = value.intervalMs;
    table["loop"] = value.loop;
    table["running"] = value.running;

    sol::table frames = lua.create_table();
    for (std::size_t index = 0; index < value.frames.size(); ++index) {
        const auto& frame = value.frames[index];
        sol::table frameTable = lua.create_table();
        frameTable["id"] = frame.id;
        frameTable["enabled"] = frame.enabled;
        frameTable["name"] = frame.name;
        sol::table fields = lua.create_table();
        for (const auto& field : descriptor.txSequenceFields) {
            const auto valueIter = frame.fields.find(field.id);
            fields[field.id] = valueIter == frame.fields.end()
                                   ? txSequenceFieldValueToLua(lua, defaultTxSequenceFieldValue(field))
                                   : txSequenceFieldValueToLua(lua, valueIter->second);
        }
        frameTable["fields"] = fields;
        frames[index + 1] = frameTable;
    }
    table["frames"] = frames;
    return sol::make_object(lua, table);
}

sol::object controlValueToLua(sol::state_view lua, const ControlDescriptor* descriptor, const ControlValue& value)
{
    if (descriptor == nullptr) {
        return sol::make_object(lua, sol::lua_nil);
    }

    if (descriptor->type == ControlType::ElfSymbolCombo) {
        const auto* symbol = std::get_if<ElfSymbolValue>(&value);
        if (symbol == nullptr || symbol->label.empty() || symbol->value.empty() || symbol->type.empty()) {
            return sol::make_object(lua, sol::lua_nil);
        }
        sol::table table = lua.create_table();
        table["label"] = symbol->label;
        table["value"] = symbol->value;
        table["type"] = symbol->type;
        return sol::make_object(lua, table);
    }

    if (descriptor->type == ControlType::ValueTable) {
        const auto* tableValue = std::get_if<ValueTableValue>(&value);
        if (tableValue == nullptr) {
            return sol::make_object(lua, sol::lua_nil);
        }
        sol::table rows = lua.create_table();
        for (std::size_t index = 0; index < descriptor->valueRows.size(); ++index) {
            const auto& row = descriptor->valueRows[index];
            sol::table item = lua.create_table();
            item["label"] = row.label;
            item["value"] = index < tableValue->rows.size() && tableValue->rows[index].set
                                ? tableValue->rows[index].value
                                : std::string();
            item["unit"] = row.unit;
            if (!row.note.empty()) {
                item["note"] = row.note;
            }
            rows[index + 1] = item;
        }
        return sol::make_object(lua, rows);
    }

    if (descriptor->type == ControlType::TxSequence) {
        const auto* sequence = std::get_if<TxSequenceValue>(&value);
        if (sequence == nullptr) {
            return sol::make_object(lua, sol::lua_nil);
        }
        return txSequenceValueToLua(lua, *descriptor, *sequence);
    }

    return std::visit(
        [&lua](const auto& current) -> sol::object {
            using ValueType = std::decay_t<decltype(current)>;
            if constexpr (std::is_same_v<ValueType, ElfSymbolValue> || std::is_same_v<ValueType, ValueTableValue> ||
                          std::is_same_v<ValueType, TxSequenceValue>) {
                return sol::make_object(lua, sol::lua_nil);
            } else {
                return sol::make_object(lua, current);
            }
        },
        value);
}

std::optional<std::vector<std::uint8_t>> bytesFromLuaObject(const sol::object& object, std::string& error)
{
    if (!object.valid() || object.get_type() == sol::type::lua_nil) {
        return std::vector<std::uint8_t>{};
    }

    if (object.get_type() == sol::type::string) {
        const auto parsed = protocol_utils::hexToBytes(object.as<std::string>());
        if (!parsed.has_value()) {
            error = "HEX 字符串解析失败";
        }
        return parsed;
    }

    if (object.get_type() == sol::type::userdata && object.is<ProtoBuffer>()) {
        return object.as<ProtoBuffer>().bytes;
    }

    if (object.get_type() != sol::type::table) {
        error = "仅支持 hex 字符串、ProtoBuffer 或 number[]";
        return std::nullopt;
    }

    const auto table = object.as<sol::table>();
    std::vector<std::uint8_t> result;
    result.reserve(table.size());
    for (std::size_t index = 1; index <= table.size(); ++index) {
        const sol::object value = table[index];
        if (!value.valid() || (!value.is<int>() && !value.is<double>())) {
            error = "number[] 中存在非数字元素";
            return std::nullopt;
        }
        int byte = value.is<int>() ? value.as<int>() : static_cast<int>(value.as<double>());
        if (byte < 0 || byte > 255) {
            error = "number[] 元素必须在 0-255 之间";
            return std::nullopt;
        }
        result.push_back(static_cast<std::uint8_t>(byte));
    }
    return result;
}

std::optional<ControlDescriptor> parseControlDescriptor(const sol::object& object, std::string& error)
{
    if (!object.is<sol::table>()) {
        error = "控件项必须是 table";
        return std::nullopt;
    }

    const auto table = object.as<sol::table>();
    const std::string typeText = readStringFieldOrPosition(table, "type", 1);
    const auto controlType = parseControlType(typeText);
    if (!controlType.has_value()) {
        error = "未知控件类型: " + typeText;
        return std::nullopt;
    }

    ControlDescriptor descriptor;
    descriptor.type = *controlType;
    descriptor.id = readStringFieldOrPosition(table, "id", 2);
    descriptor.label = readStringFieldOrPosition(table, "label", 3);
    if (!applyControlLabelPosition(descriptor, table, error) || !validateControlIdentity(descriptor, error) ||
        !applyControlCompactLabelConfig(descriptor, table, error) ||
        !applyControlTypeConfig(descriptor, table, error)) {
        return std::nullopt;
    }

    return descriptor;
}

std::optional<std::vector<ControlDescriptor>> parseControlList(const sol::object& object, std::string& error)
{
    if (!object.is<sol::table>()) {
        error = "控件列表必须返回 table";
        return std::nullopt;
    }

    const auto table = object.as<sol::table>();
    std::vector<ControlDescriptor> controls;
    controls.reserve(table.size());
    for (std::size_t index = 1; index <= table.size(); ++index) {
        auto descriptor = parseControlDescriptor(table[index], error);
        if (!descriptor.has_value()) {
            return std::nullopt;
        }
        controls.push_back(std::move(*descriptor));
    }
    return controls;
}

bool readOptionalBoolField(
    const sol::table& table, std::string_view field, bool defaultValue, const std::string& path, std::string& error)
{
    const sol::object value = table[std::string(field)];
    if (!value.valid() || value.get_type() == sol::type::lua_nil) {
        return defaultValue;
    }
    if (!value.is<bool>()) {
        error = path + "." + std::string(field) + " 必须是 boolean";
        return defaultValue;
    }
    return value.as<bool>();
}

std::optional<float> readOptionalFloatField(
    const sol::table& table, std::string_view field, float defaultValue, const std::string& path, std::string& error)
{
    const sol::object value = table[std::string(field)];
    if (!value.valid() || value.get_type() == sol::type::lua_nil) {
        return defaultValue;
    }
    if (!value.is<double>() && !value.is<int>()) {
        error = path + "." + std::string(field) + " 必须是 number";
        return std::nullopt;
    }
    const double number = value.is<double>() ? value.as<double>() : static_cast<double>(value.as<int>());
    if (!std::isfinite(number) || number < 0.0) {
        error = path + "." + std::string(field) + " 必须是非负 number";
        return std::nullopt;
    }
    return static_cast<float>(number);
}

bool hasLuaTableField(const sol::table& table, std::string_view field)
{
    const sol::object value = table[std::string(field)];
    return value.valid() && value.get_type() != sol::type::lua_nil;
}

bool readOptionalPositiveFloatField(const sol::table& table,
                                    std::string_view field,
                                    const std::string& path,
                                    std::optional<float>& result,
                                    std::string& error)
{
    const sol::object value = table[std::string(field)];
    if (!value.valid() || value.get_type() == sol::type::lua_nil) {
        return true;
    }
    if (!value.is<double>() && !value.is<int>()) {
        error = path + "." + std::string(field) + " 必须是 number";
        return false;
    }
    const double number = value.is<double>() ? value.as<double>() : static_cast<double>(value.as<int>());
    if (!std::isfinite(number) || number <= 0.0) {
        error = path + "." + std::string(field) + " 必须是正数";
        return false;
    }
    result = static_cast<float>(number);
    return true;
}

bool readLayoutControlWidthFields(const sol::table& table,
                                  const std::string& path,
                                  LayoutNodeDescriptor& node,
                                  std::string& error)
{
    if (!readOptionalPositiveFloatField(table, "min_width", path, node.minWidth, error) ||
        !readOptionalPositiveFloatField(table, "max_width", path, node.maxWidth, error)) {
        return false;
    }
    if (node.minWidth.has_value() && node.maxWidth.has_value() && *node.minWidth > *node.maxWidth) {
        error = path + ".min_width 不能大于 max_width";
        return false;
    }
    node.fillWidth = readOptionalBoolField(table, "fill_width", false, path, error);
    if (!error.empty()) {
        return false;
    }
    return true;
}

bool registerLayoutControlUse(const DockDescriptor& dock,
                              const std::unordered_map<std::string, std::size_t>& controlsById,
                              std::unordered_set<std::string>& usedControls,
                              const std::string& controlId,
                              const std::string& path,
                              LayoutNodeDescriptor& node,
                              std::string& error)
{
    if (controlId.empty()) {
        error = path + ".id 不能为空";
        return false;
    }
    const auto controlIter = controlsById.find(controlId);
    if (controlIter == controlsById.end()) {
        error = "dock '" + dock.id + "' 的 layout 引用了未声明控件 '" + controlId + "'";
        return false;
    }
    if (!usedControls.emplace(controlId).second) {
        error = "dock '" + dock.id + "' 的 layout 重复引用控件 '" + controlId + "'";
        return false;
    }
    node.controlId = controlId;
    node.controlIndex = controlIter->second;
    return true;
}

std::optional<LayoutNodeDescriptor> parseLayoutControlShortcutNode(
    const DockDescriptor& dock,
    const std::unordered_map<std::string, std::size_t>& controlsById,
    std::unordered_set<std::string>& usedControls,
    const std::string& controlId,
    const sol::table* table,
    const std::string& path,
    std::string& error)
{
    LayoutNodeDescriptor node;
    node.kind = LayoutNodeKind::Control;
    if (!registerLayoutControlUse(dock, controlsById, usedControls, controlId, path, node, error)) {
        return std::nullopt;
    }
    if (table != nullptr && !readLayoutControlWidthFields(*table, path, node, error)) {
        return std::nullopt;
    }
    return node;
}

bool isLuaStringArray(const sol::table& table)
{
    if (table.size() == 0) {
        return false;
    }
    for (std::size_t index = 1; index <= table.size(); ++index) {
        const sol::object item = table[index];
        if (!item.is<std::string>()) {
            return false;
        }
    }
    return true;
}

std::optional<LayoutNodeDescriptor> parseLayoutStringArrayFlow(
    const DockDescriptor& dock,
    const sol::table& table,
    const std::unordered_map<std::string, std::size_t>& controlsById,
    std::unordered_set<std::string>& usedControls,
    const std::string& path,
    std::string& error)
{
    LayoutNodeDescriptor node;
    node.kind = LayoutNodeKind::Flow;
    node.children.reserve(table.size());
    for (std::size_t index = 1; index <= table.size(); ++index) {
        const sol::object item = table[index];
        auto child = parseLayoutControlShortcutNode(dock,
                                                    controlsById,
                                                    usedControls,
                                                    item.as<std::string>(),
                                                    nullptr,
                                                    path + "[" + std::to_string(index) + "]",
                                                    error);
        if (!child.has_value()) {
            return std::nullopt;
        }
        node.children.push_back(std::move(*child));
    }
    return node;
}

std::optional<LayoutNodeDescriptor> parseLayoutNode(const DockDescriptor& dock,
                                                    const sol::object& object,
                                                    const std::unordered_map<std::string, std::size_t>& controlsById,
                                                    std::unordered_set<std::string>& usedControls,
                                                    const std::string& path,
                                                    std::string& error);

std::optional<std::vector<LayoutNodeDescriptor>> parseLayoutSequenceChildren(
    const DockDescriptor& dock,
    const sol::table& table,
    const std::unordered_map<std::string, std::size_t>& controlsById,
    std::unordered_set<std::string>& usedControls,
    const std::string& path,
    std::string& error)
{
    if (table.size() == 0) {
        error = path + " 必须声明 children、controls 或数组子节点";
        return std::nullopt;
    }

    std::vector<LayoutNodeDescriptor> children;
    children.reserve(table.size());
    for (std::size_t index = 1; index <= table.size(); ++index) {
        auto child = parseLayoutNode(
            dock, table[index], controlsById, usedControls, path + "[" + std::to_string(index) + "]", error);
        if (!child.has_value()) {
            return std::nullopt;
        }
        children.push_back(std::move(*child));
    }
    return children;
}

std::optional<std::vector<LayoutNodeDescriptor>> parseLayoutChildren(
    const DockDescriptor& dock,
    const sol::table& table,
    const std::unordered_map<std::string, std::size_t>& controlsById,
    std::unordered_set<std::string>& usedControls,
    const std::string& path,
    std::string& error)
{
    const sol::object childrenObject = table["children"];
    if (!childrenObject.valid() || childrenObject.get_type() == sol::type::lua_nil ||
        !childrenObject.is<sol::table>()) {
        error = path + ".children 必须是非空数组";
        return std::nullopt;
    }
    const auto childrenTable = childrenObject.as<sol::table>();
    if (childrenTable.size() == 0) {
        error = path + ".children 必须是非空数组";
        return std::nullopt;
    }

    std::vector<LayoutNodeDescriptor> children;
    children.reserve(childrenTable.size());
    for (std::size_t index = 1; index <= childrenTable.size(); ++index) {
        auto child = parseLayoutNode(dock,
                                     childrenTable[index],
                                     controlsById,
                                     usedControls,
                                     path + ".children[" + std::to_string(index) + "]",
                                     error);
        if (!child.has_value()) {
            return std::nullopt;
        }
        children.push_back(std::move(*child));
    }
    return children;
}

std::optional<std::vector<LayoutNodeDescriptor>> parseLayoutControlShortcutChildren(
    const DockDescriptor& dock,
    const sol::table& table,
    const std::unordered_map<std::string, std::size_t>& controlsById,
    std::unordered_set<std::string>& usedControls,
    const std::string& path,
    std::string& error)
{
    const sol::object controlsObject = table["controls"];
    if (!controlsObject.valid() || controlsObject.get_type() == sol::type::lua_nil ||
        !controlsObject.is<sol::table>()) {
        error = path + ".controls 必须是非空字符串数组";
        return std::nullopt;
    }
    const auto controlsTable = controlsObject.as<sol::table>();
    if (controlsTable.size() == 0) {
        error = path + ".controls 必须是非空字符串数组";
        return std::nullopt;
    }

    std::vector<LayoutNodeDescriptor> children;
    children.reserve(controlsTable.size());
    for (std::size_t index = 1; index <= controlsTable.size(); ++index) {
        const sol::object controlObject = controlsTable[index];
        const std::string controlPath = path + ".controls[" + std::to_string(index) + "]";
        if (!controlObject.is<std::string>()) {
            error = controlPath + " 必须是字符串";
            return std::nullopt;
        }

        LayoutNodeDescriptor child;
        child.kind = LayoutNodeKind::Control;
        // controls 简写只展开成 control 子节点，仍复用统一的控件引用校验。
        if (!registerLayoutControlUse(
                dock, controlsById, usedControls, controlObject.as<std::string>(), controlPath, child, error)) {
            return std::nullopt;
        }
        children.push_back(std::move(child));
    }
    return children;
}

std::optional<std::vector<LayoutNodeDescriptor>> parseLayoutContainerChildren(
    const DockDescriptor& dock,
    const sol::table& table,
    const std::unordered_map<std::string, std::size_t>& controlsById,
    std::unordered_set<std::string>& usedControls,
    const std::string& path,
    std::string& error)
{
    const bool hasChildren = hasLuaTableField(table, "children");
    const bool hasControls = hasLuaTableField(table, "controls");
    if (hasChildren && hasControls) {
        error = path + " 不能同时声明 children 和 controls";
        return std::nullopt;
    }
    if (hasControls) {
        return parseLayoutControlShortcutChildren(dock, table, controlsById, usedControls, path, error);
    }
    return parseLayoutChildren(dock, table, controlsById, usedControls, path, error);
}

bool validateInlineGroupChildren(const std::vector<LayoutNodeDescriptor>& children,
                                 const std::string& path,
                                 std::string& error)
{
    for (const auto& child : children) {
        if (child.kind == LayoutNodeKind::Control || child.kind == LayoutNodeKind::Text) {
            continue;
        }
        error = path + " inline_group 只允许 control 或 text 子节点";
        return false;
    }
    return true;
}

std::optional<std::size_t> readLayoutTableColumnCount(const sol::table& table,
                                                      const std::string& path,
                                                      std::string& error)
{
    const sol::object columnsObject = table["columns"];
    if (!columnsObject.valid() || columnsObject.get_type() == sol::type::lua_nil || !columnsObject.is<double>()) {
        error = path + ".columns 必须是 >= 1 的整数";
        return std::nullopt;
    }
    const auto columnsValue = columnsObject.as<double>();
    if (!std::isfinite(columnsValue) || columnsValue < 1.0 || std::floor(columnsValue) != columnsValue) {
        error = path + ".columns 必须是 >= 1 的整数";
        return std::nullopt;
    }
    return static_cast<std::size_t>(columnsValue);
}

std::optional<std::vector<std::vector<LayoutNodeDescriptor>>> parseLayoutTableRows(
    const DockDescriptor& dock,
    const sol::table& table,
    const std::unordered_map<std::string, std::size_t>& controlsById,
    std::unordered_set<std::string>& usedControls,
    std::size_t columns,
    const std::string& path,
    std::string& error)
{
    const sol::object rowsObject = table["rows"];
    if (!rowsObject.valid() || rowsObject.get_type() == sol::type::lua_nil || !rowsObject.is<sol::table>()) {
        error = path + ".rows 必须是非空数组";
        return std::nullopt;
    }
    const auto rowsTable = rowsObject.as<sol::table>();
    if (rowsTable.size() == 0) {
        error = path + ".rows 必须是非空数组";
        return std::nullopt;
    }

    std::vector<std::vector<LayoutNodeDescriptor>> rows;
    rows.reserve(rowsTable.size());
    for (std::size_t rowIndex = 1; rowIndex <= rowsTable.size(); ++rowIndex) {
        const sol::object rowObject = rowsTable[rowIndex];
        if (!rowObject.is<sol::table>()) {
            error = path + ".rows[" + std::to_string(rowIndex) + "] 必须是 table";
            return std::nullopt;
        }
        const auto rowTable = rowObject.as<sol::table>();
        if (rowTable.size() > columns) {
            error = path + ".rows[" + std::to_string(rowIndex) + "] 的单元格数量不能超过 columns";
            return std::nullopt;
        }
        std::vector<LayoutNodeDescriptor> row;
        row.reserve(rowTable.size());
        for (std::size_t cellIndex = 1; cellIndex <= rowTable.size(); ++cellIndex) {
            auto cell =
                parseLayoutNode(dock,
                                rowTable[cellIndex],
                                controlsById,
                                usedControls,
                                path + ".rows[" + std::to_string(rowIndex) + "][" + std::to_string(cellIndex) + "]",
                                error);
            if (!cell.has_value()) {
                return std::nullopt;
            }
            row.push_back(std::move(*cell));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::optional<LayoutNodeDescriptor> parseLayoutTableNode(
    const DockDescriptor& dock,
    const sol::table& table,
    const std::unordered_map<std::string, std::size_t>& controlsById,
    std::unordered_set<std::string>& usedControls,
    const std::string& path,
    std::string& error)
{
    LayoutNodeDescriptor node;
    node.kind = LayoutNodeKind::Table;

    const auto columns = readLayoutTableColumnCount(table, path, error);
    if (!columns.has_value()) {
        return std::nullopt;
    }
    node.columns = *columns;
    node.borders = readOptionalBoolField(table, "borders", false, path, error);
    node.resizable = readOptionalBoolField(table, "resizable", true, path, error);
    node.rowBg = readOptionalBoolField(table, "row_bg", false, path, error);
    if (!error.empty()) {
        return std::nullopt;
    }
    node.sizing = table.get_or("sizing", std::string("stretch"));
    if (node.sizing != "stretch") {
        error = path + ".sizing 目前仅支持 'stretch'";
        return std::nullopt;
    }

    auto rows = parseLayoutTableRows(dock, table, controlsById, usedControls, node.columns, path, error);
    if (!rows.has_value()) {
        return std::nullopt;
    }
    node.rows = std::move(*rows);
    return node;
}

std::optional<LayoutNodeDescriptor> parseImplicitLayoutNode(
    const DockDescriptor& dock,
    const sol::table& table,
    const std::unordered_map<std::string, std::size_t>& controlsById,
    std::unordered_set<std::string>& usedControls,
    const std::string& path,
    std::string& error)
{
    if (const sol::object idObject = table["id"]; idObject.valid() && idObject.get_type() != sol::type::lua_nil) {
        if (!idObject.is<std::string>()) {
            error = path + ".id 必须是字符串";
            return std::nullopt;
        }
        return parseLayoutControlShortcutNode(
            dock, controlsById, usedControls, idObject.as<std::string>(), &table, path, error);
    }
    if (const sol::object textObject = table["text"];
        textObject.valid() && textObject.get_type() != sol::type::lua_nil) {
        if (!textObject.is<std::string>()) {
            error = path + ".text 必须是字符串";
            return std::nullopt;
        }
        LayoutNodeDescriptor node;
        node.kind = LayoutNodeKind::Text;
        node.text = textObject.as<std::string>();
        return node;
    }
    if (const sol::object separatorObject = table["separator"];
        separatorObject.valid() && separatorObject.get_type() != sol::type::lua_nil) {
        if (!separatorObject.is<bool>() || !separatorObject.as<bool>()) {
            error = path + ".separator 必须是 true";
            return std::nullopt;
        }
        LayoutNodeDescriptor node;
        node.kind = LayoutNodeKind::Separator;
        return node;
    }
    if (const sol::object spacerObject = table["spacer"];
        spacerObject.valid() && spacerObject.get_type() != sol::type::lua_nil) {
        if (!spacerObject.is<bool>() || !spacerObject.as<bool>()) {
            error = path + ".spacer 必须是 true";
            return std::nullopt;
        }
        LayoutNodeDescriptor node;
        node.kind = LayoutNodeKind::Spacer;
        return node;
    }
    if (isLuaStringArray(table)) {
        return parseLayoutStringArrayFlow(dock, table, controlsById, usedControls, path, error);
    }

    LayoutNodeDescriptor node;
    node.kind = LayoutNodeKind::Column;
    std::optional<std::vector<LayoutNodeDescriptor>> children;
    if (hasLuaTableField(table, "children") || hasLuaTableField(table, "controls")) {
        children = parseLayoutContainerChildren(dock, table, controlsById, usedControls, path, error);
    } else {
        // 无 type 的 layout 容器默认按 column 处理，保持严格全覆盖校验不变。
        children = parseLayoutSequenceChildren(dock, table, controlsById, usedControls, path, error);
    }
    if (!children.has_value()) {
        return std::nullopt;
    }
    node.children = std::move(*children);
    return node;
}

std::optional<LayoutNodeDescriptor> parseTypedLayoutNode(
    const DockDescriptor& dock,
    const sol::table& table,
    const std::unordered_map<std::string, std::size_t>& controlsById,
    std::unordered_set<std::string>& usedControls,
    const std::string& type,
    const std::string& path,
    std::string& error)
{
    LayoutNodeDescriptor node;
    if (type == "column" || type == "flow") {
        node.kind = type == "column" ? LayoutNodeKind::Column : LayoutNodeKind::Flow;
        if (type == "flow") {
            const auto spacing = readOptionalFloatField(table, "spacing", node.spacing, path, error);
            const auto runSpacing = readOptionalFloatField(table, "run_spacing", node.runSpacing, path, error);
            if (!spacing.has_value() || !runSpacing.has_value()) {
                return std::nullopt;
            }
            node.spacing = *spacing;
            node.runSpacing = *runSpacing;
        }
        auto children = parseLayoutContainerChildren(dock, table, controlsById, usedControls, path, error);
        if (!children.has_value()) {
            return std::nullopt;
        }
        node.children = std::move(*children);
        return node;
    }
    if (type == "inline_group") {
        node.kind = LayoutNodeKind::InlineGroup;
        const auto spacing = readOptionalFloatField(table, "spacing", node.spacing, path, error);
        if (!spacing.has_value()) {
            return std::nullopt;
        }
        node.spacing = *spacing;
        if (!readOptionalPositiveFloatField(table, "min_width", path, node.minWidth, error)) {
            return std::nullopt;
        }
        node.fillWidth = readOptionalBoolField(table, "fill_width", false, path, error);
        if (!error.empty()) {
            return std::nullopt;
        }
        if (hasLuaTableField(table, "max_width")) {
            error = path + ".max_width 不支持 inline_group";
            return std::nullopt;
        }
        auto children = parseLayoutContainerChildren(dock, table, controlsById, usedControls, path, error);
        if (!children.has_value()) {
            return std::nullopt;
        }
        if (!validateInlineGroupChildren(*children, path, error)) {
            return std::nullopt;
        }
        node.children = std::move(*children);
        return node;
    }
    if (type == "group" || type == "collapse") {
        const sol::object titleObject = table["title"];
        if (!titleObject.valid() || titleObject.get_type() == sol::type::lua_nil || !titleObject.is<std::string>()) {
            error = path + ".title 必须是字符串";
            return std::nullopt;
        }
        node.kind = type == "group" ? LayoutNodeKind::Group : LayoutNodeKind::Collapse;
        node.title = titleObject.as<std::string>();
        if (node.title.empty()) {
            error = path + ".title 不能为空";
            return std::nullopt;
        }
        if (type == "collapse") {
            node.defaultOpen = readOptionalBoolField(table, "default_open", true, path, error);
            if (!error.empty()) {
                return std::nullopt;
            }
        }
        auto children = parseLayoutChildren(dock, table, controlsById, usedControls, path, error);
        if (!children.has_value()) {
            return std::nullopt;
        }
        node.children = std::move(*children);
        return node;
    }
    if (type == "control") {
        node.kind = LayoutNodeKind::Control;
        const sol::object idObject = table["id"];
        if (!idObject.valid() || idObject.get_type() == sol::type::lua_nil || !idObject.is<std::string>()) {
            error = path + ".id 必须是字符串";
            return std::nullopt;
        }
        if (!registerLayoutControlUse(
                dock, controlsById, usedControls, idObject.as<std::string>(), path, node, error)) {
            return std::nullopt;
        }
        if (!readLayoutControlWidthFields(table, path, node, error)) {
            return std::nullopt;
        }
        return node;
    }
    if (type == "text") {
        node.kind = LayoutNodeKind::Text;
        const sol::object textObject = table["text"];
        if (!textObject.valid() || textObject.get_type() == sol::type::lua_nil || !textObject.is<std::string>()) {
            error = path + ".text 必须是字符串";
            return std::nullopt;
        }
        node.text = textObject.as<std::string>();
        return node;
    }
    if (type == "separator") {
        node.kind = LayoutNodeKind::Separator;
        return node;
    }
    if (type == "spacer") {
        node.kind = LayoutNodeKind::Spacer;
        return node;
    }
    if (type == "table") {
        return parseLayoutTableNode(dock, table, controlsById, usedControls, path, error);
    }

    error = path + ".type 不支持: " + type;
    return std::nullopt;
}

std::optional<LayoutNodeDescriptor> parseLayoutNode(const DockDescriptor& dock,
                                                    const sol::object& object,
                                                    const std::unordered_map<std::string, std::size_t>& controlsById,
                                                    std::unordered_set<std::string>& usedControls,
                                                    const std::string& path,
                                                    std::string& error)
{
    if (object.is<std::string>()) {
        return parseLayoutControlShortcutNode(
            dock, controlsById, usedControls, object.as<std::string>(), nullptr, path, error);
    }
    if (!object.is<sol::table>()) {
        error = path + " 必须是 table";
        return std::nullopt;
    }

    const auto table = object.as<sol::table>();
    const sol::object typeObject = table["type"];
    if (typeObject.valid() && typeObject.get_type() != sol::type::lua_nil && !typeObject.is<std::string>()) {
        error = path + ".type 必须是字符串";
        return std::nullopt;
    }
    if (!typeObject.valid() || typeObject.get_type() == sol::type::lua_nil) {
        return parseImplicitLayoutNode(dock, table, controlsById, usedControls, path, error);
    }
    return parseTypedLayoutNode(dock, table, controlsById, usedControls, typeObject.as<std::string>(), path, error);
}

std::optional<DockLayoutDescriptor> parseDockLayout(const DockDescriptor& dock,
                                                    const sol::object& object,
                                                    std::string& error)
{
    if (!object.is<sol::table>()) {
        error = "dock '" + dock.id + "' 的 layout 必须是 table";
        return std::nullopt;
    }

    std::unordered_map<std::string, std::size_t> controlsById;
    controlsById.reserve(dock.controls.size());
    for (std::size_t index = 0; index < dock.controls.size(); ++index) {
        const auto& control = dock.controls[index];
        if (!controlsById.emplace(control.id, index).second) {
            error = "dock '" + dock.id + "' 的控件 id 重复: " + control.id;
            return std::nullopt;
        }
    }

    std::unordered_set<std::string> usedControls;
    usedControls.reserve(dock.controls.size());
    auto root = parseLayoutNode(dock, object, controlsById, usedControls, "dock '" + dock.id + "' 的 layout", error);
    if (!root.has_value()) {
        return std::nullopt;
    }

    if (usedControls.size() != dock.controls.size()) {
        std::vector<std::string> missingControls;
        for (const auto& control : dock.controls) {
            if (!usedControls.contains(control.id)) {
                missingControls.push_back(control.id);
            }
        }
        std::ostringstream stream;
        stream << "dock '" << dock.id << "' 的 layout 缺少控件:";
        for (const auto& controlId : missingControls) {
            stream << ' ' << controlId;
        }
        error = stream.str();
        return std::nullopt;
    }

    DockLayoutDescriptor layout;
    layout.root = std::move(*root);
    return layout;
}

std::optional<DockDescriptor> parseSingleDockDescriptor(const sol::object& object, std::string& error)
{
    if (!object.is<sol::table>()) {
        error = "ui() 每个 dock 都必须是 table";
        return std::nullopt;
    }

    const auto dockEntry = object.as<sol::table>();
    DockDescriptor dock;
    dock.id = readStringField(dockEntry, "id");
    dock.title = readStringField(dockEntry, "title");
    dock.anchor = readStringField(dockEntry, "anchor", "left_bottom");
    dock.tabGroup = readStringField(dockEntry, "tab_group");
    if (dock.id.empty() || dock.title.empty()) {
        error = "dock 必须提供 id 和 title";
        return std::nullopt;
    }
    if (!isValidDockAnchor(dock.anchor)) {
        error = "dock anchor 不支持: " + dock.anchor;
        return std::nullopt;
    }

    const sol::object controlsObject = dockEntry["controls"];
    auto controls = parseControlList(controlsObject, error);
    if (!controls.has_value()) {
        return std::nullopt;
    }
    dock.controls = std::move(*controls);

    const sol::object layoutObject = dockEntry["layout"];
    if (layoutObject.valid() && layoutObject.get_type() != sol::type::lua_nil) {
        auto layout = parseDockLayout(dock, layoutObject, error);
        if (!layout.has_value()) {
            return std::nullopt;
        }
        dock.layout = std::move(*layout);
    }
    return dock;
}

std::optional<std::vector<DockDescriptor>> parseDockDescriptorList(const sol::object& object, std::string& error)
{
    if (!object.is<sol::table>()) {
        error = "ui() 必须返回 table";
        return std::nullopt;
    }

    const auto dockTable = object.as<sol::table>();
    if (hasLuaTableField(dockTable, "id") || hasLuaTableField(dockTable, "title") ||
        hasLuaTableField(dockTable, "controls") || hasLuaTableField(dockTable, "layout")) {
        auto dock = parseSingleDockDescriptor(object, error);
        if (!dock.has_value()) {
            return std::nullopt;
        }
        return std::vector<DockDescriptor>{std::move(*dock)};
    }

    std::vector<DockDescriptor> docks;
    docks.reserve(dockTable.size());
    for (std::size_t index = 1; index <= dockTable.size(); ++index) {
        auto dock = parseSingleDockDescriptor(dockTable[index], error);
        if (!dock.has_value()) {
            return std::nullopt;
        }
        docks.push_back(std::move(*dock));
    }
    return docks;
}

std::optional<std::vector<DockDescriptor>> parseUiDockDescriptors(const sol::object& uiObject, std::string& error)
{
    if (!uiObject.is<sol::protected_function>()) {
        error = "ui 必须是 function";
        return std::nullopt;
    }

    auto uiFunction = uiObject.as<sol::protected_function>();
    auto uiResult = uiFunction();
    if (!uiResult.valid()) {
        error = "ui() 执行失败";
        return std::nullopt;
    }

    const sol::object docksObject = uiResult.get<sol::object>();
    return parseDockDescriptorList(docksObject, error);
}

std::optional<std::vector<DockDescriptor>> parseDefaultDockDescriptors(const sol::object& controlsObject,
                                                                       std::string& error)
{
    if (!controlsObject.valid() || controlsObject.get_type() == sol::type::lua_nil) {
        return std::vector<DockDescriptor>{};
    }
    if (!controlsObject.is<sol::protected_function>()) {
        error = "controls 必须是 function";
        return std::nullopt;
    }

    auto controlsFunction = controlsObject.as<sol::protected_function>();
    auto controlsResult = controlsFunction();
    if (!controlsResult.valid()) {
        error = "controls() 执行失败";
        return std::nullopt;
    }

    const sol::object descriptorsObject = controlsResult.get<sol::object>();
    auto controls = parseControlList(descriptorsObject, error);
    if (!controls.has_value()) {
        return std::nullopt;
    }

    DockDescriptor dock;
    dock.id = kDefaultDockId;
    dock.title = kDefaultDockTitle;
    dock.controls = std::move(*controls);
    return std::vector<DockDescriptor>{std::move(dock)};
}

std::optional<std::vector<DockDescriptor>> parseDockDescriptors(sol::state_view lua, std::string& error)
{
    const sol::object uiObject = lua["ui"];
    if (uiObject.valid() && uiObject.get_type() != sol::type::lua_nil) {
        return parseUiDockDescriptors(uiObject, error);
    }

    return parseDefaultDockDescriptors(lua["controls"], error);
}

sol::table makeContextTable(sol::state_view lua, const transport::ConnectionContext& connection)
{
    sol::table table = lua.create_table();
    table["kind"] = kindName(connection.kind);
    table["endpoint"] = connection.endpoint;
    table["connection_id"] = connection.connectionId;
    table["timestamp_ms"] = connection.timestampMs;
    table["ready_for_io"] = connection.readyForIo;
    return table;
}

sol::table makeBytesTable(sol::state_view lua, const std::vector<std::uint8_t>& bytes)
{
    sol::table table = lua.create_table(static_cast<int>(bytes.size()), 0);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        table[index + 1] = bytes[index];
    }
    return table;
}

sol::table makeTxEventTable(sol::state_view lua, const TxEvent& event)
{
    sol::table table = lua.create_table();
    table["id"] = event.id;
    table["kind"] = txKindName(event.kind);
    table["state"] = txEventStateName(event.state);
    table["tag"] = event.tag;
    table["bytes"] = static_cast<int>(event.bytes);
    table["queued_ms"] = event.queuedMs;
    table["finished_ms"] = event.finishedMs;
    if (event.guarded) {
        table["guarded"] = true;
        table["attempt"] = static_cast<int>(event.attempt);
        table["max_attempts"] = static_cast<int>(event.maxAttempts);
    }
    if (event.guardState.has_value()) {
        table["guard_state"] = *event.guardState;
    }
    if (event.fileJobId != 0) {
        table["file_job_id"] = event.fileJobId;
        table["offset"] = event.offset;
        table["total"] = event.total;
        table["progress"] = event.progress;
    }
    if (event.error.has_value()) {
        table["error"] = *event.error;
    }
    return table;
}

sol::table makeDialogEventTable(sol::state_view lua, const DialogEvent& event)
{
    sol::table table = lua.create_table();
    table["id"] = event.id;
    table["kind"] = dialogKindName(event.kind);
    table["state"] = event.state;
    table["title"] = event.title;
    table["message"] = event.message;
    table["level"] = event.level;
    table["dedupe_key"] = event.dedupeKey;
    table["timestamp_ms"] = event.timestampMs;
    if (event.confirmed.has_value()) {
        table["confirmed"] = *event.confirmed;
    }
    return table;
}

sol::table makeFileDialogEventTable(sol::state_view lua, const FileDialogEvent& event)
{
    sol::table table = lua.create_table();
    table["id"] = event.id;
    table["kind"] = fileDialogKindName(event.kind);
    table["state"] = event.state;
    table["timestamp_ms"] = event.timestampMs;
    if (!event.path.empty()) {
        table["path"] = event.path;
    }
    if (!event.error.empty()) {
        table["error"] = event.error;
    }
    return table;
}

std::filesystem::path canonicalPath(const std::filesystem::path& path)
{
    std::error_code errorCode;
    auto canonical = std::filesystem::weakly_canonical(std::filesystem::absolute(path), errorCode);
    if (errorCode) {
        canonical = std::filesystem::absolute(path).lexically_normal();
    }
    return canonical;
}

std::string comparablePath(std::filesystem::path path)
{
    path = path.lexically_normal();
    auto text = path.generic_string();
#if defined(_WIN32)
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
#endif
    return text;
}

bool isSameOrChildPath(const std::filesystem::path& root, const std::filesystem::path& path, bool recursive)
{
    const auto rootText = comparablePath(root);
    const auto pathText = comparablePath(path);
    if (rootText == pathText) {
        return true;
    }
    if (!recursive || rootText.empty()) {
        return false;
    }
    const auto prefix = rootText.back() == '/' ? rootText : rootText + "/";
    return pathText.rfind(prefix, 0) == 0;
}

std::size_t positiveSizeOrDefault(std::size_t value, std::size_t fallback)
{
    return value == 0 ? fallback : value;
}

std::string protectedCallError(sol::protected_function_result& result)
{
    sol::error error = result;
    return error.what();
}

std::optional<double> luaNumberField(const sol::table& table, const char* key)
{
    const sol::object object = table[key];
    if (!object.valid() || object.get_type() == sol::type::lua_nil || !object.is<double>()) {
        return std::nullopt;
    }
    return object.as<double>();
}

std::optional<std::string> luaStringField(const sol::table& table, const char* key)
{
    const sol::object object = table[key];
    if (!object.valid() || object.get_type() == sol::type::lua_nil || !object.is<std::string>()) {
        return std::nullopt;
    }
    return object.as<std::string>();
}

std::optional<bool> luaBoolField(const sol::table& table, const char* key)
{
    const sol::object object = table[key];
    if (!object.valid() || object.get_type() == sol::type::lua_nil || !object.is<bool>()) {
        return std::nullopt;
    }
    return object.as<bool>();
}

std::optional<DialogWindowOptions> luaDialogWindowOptions(const sol::table& table, std::string& error)
{
    const sol::object windowObject = table["window"];
    if (!windowObject.valid() || windowObject.get_type() == sol::type::lua_nil) {
        return DialogWindowOptions{};
    }
    if (!windowObject.is<sol::table>()) {
        error = "window 必须是 table";
        return std::nullopt;
    }

    const sol::table windowTable = windowObject.as<sol::table>();
    DialogWindowOptions options{};
    options.width = luaNumberField(windowTable, "width");
    options.height = luaNumberField(windowTable, "height");
    options.x = luaNumberField(windowTable, "x");
    options.y = luaNumberField(windowTable, "y");
    options.resizable = luaBoolField(windowTable, "resizable").value_or(true);
    options.movable = luaBoolField(windowTable, "movable").value_or(true);
    options.autoResize = luaBoolField(windowTable, "auto_resize").value_or(false);

    auto invalidPositiveNumber = [&](const std::optional<double>& value, const char* field) -> bool {
        if (!value.has_value()) {
            return false;
        }
        if (!std::isfinite(*value) || *value <= 0.0) {
            error = std::string("window.") + field + " 必须是正数";
            return true;
        }
        return false;
    };
    auto invalidFiniteNumber = [&](const std::optional<double>& value, const char* field) -> bool {
        if (!value.has_value()) {
            return false;
        }
        if (!std::isfinite(*value)) {
            error = std::string("window.") + field + " 必须是有限数值";
            return true;
        }
        return false;
    };

    if (invalidPositiveNumber(options.width, "width") || invalidPositiveNumber(options.height, "height") ||
        invalidFiniteNumber(options.x, "x") || invalidFiniteNumber(options.y, "y")) {
        return std::nullopt;
    }
    return options;
}

std::optional<FileDialogKind> resolveFileDialogKind(FileDialogKind kind, const sol::table& table, std::string& error)
{
    if (kind != FileDialogKind::OpenFile) {
        return kind;
    }

    const auto mode = luaStringField(table, "mode").value_or("open");
    if (mode == "save") {
        return FileDialogKind::SaveFile;
    }
    if (mode == "open") {
        return FileDialogKind::OpenFile;
    }

    error = "mode 必须是 open 或 save";
    return std::nullopt;
}

transport::ConnectionContext fileDialogConnectionContext(
    const std::optional<transport::ConnectionContext>& activeConnection, std::uint64_t createdAtMs)
{
    if (activeConnection.has_value()) {
        return *activeConnection;
    }

    transport::ConnectionContext connection{};
    connection.timestampMs = createdAtMs;
    connection.endpoint = "detached";
    return connection;
}

FileDialogFilter parseFileDialogFilter(const sol::table& filterTable)
{
    FileDialogFilter filter;
    filter.name = luaStringField(filterTable, "name").value_or("Files");

    const sol::object patternsObject = filterTable["patterns"];
    if (patternsObject.is<sol::table>()) {
        const auto patterns = patternsObject.as<sol::table>();
        for (std::size_t patternIndex = 1; patternIndex <= patterns.size(); ++patternIndex) {
            const sol::object pattern = patterns[patternIndex];
            if (pattern.is<std::string>()) {
                filter.patterns.push_back(pattern.as<std::string>());
            }
        }
    }
    return filter;
}

std::optional<std::vector<FileDialogFilter>> parseFileDialogFilters(const sol::table& table, std::string& error)
{
    std::vector<FileDialogFilter> parsed;
    const sol::object filtersObject = table["filters"];
    if (!filtersObject.valid() || filtersObject.get_type() == sol::type::lua_nil) {
        return parsed;
    }
    if (!filtersObject.is<sol::table>()) {
        error = "filters 必须是数组";
        return std::nullopt;
    }

    const auto filters = filtersObject.as<sol::table>();
    for (std::size_t index = 1; index <= filters.size(); ++index) {
        const sol::object filterObject = filters[index];
        if (!filterObject.is<sol::table>()) {
            error = "filters 元素必须是 table";
            return std::nullopt;
        }
        parsed.push_back(parseFileDialogFilter(filterObject.as<sol::table>()));
    }
    return parsed;
}

FileDialogRequest makeFileDialogRequest(std::uint64_t id,
                                        FileDialogKind kind,
                                        transport::ConnectionContext connection,
                                        const sol::table& table,
                                        std::vector<FileDialogFilter> filters,
                                        std::uint64_t createdAtMs)
{
    return FileDialogRequest{
        .id = id,
        .kind = kind,
        .connection = std::move(connection),
        .title = luaStringField(table, "title").value_or(kind == FileDialogKind::OpenDir ? "选择目录" : "选择文件"),
        .defaultPath = luaStringField(table, "default_path").value_or("."),
        .filters = std::move(filters),
        .createdAtMs = createdAtMs,
    };
}

std::optional<std::int64_t> luaIntegerValue(const sol::object& object)
{
    if (!object.valid() || object.get_type() == sol::type::lua_nil) {
        return std::nullopt;
    }
    if (object.is<int>()) {
        return object.as<int>();
    }
    if (object.is<double>()) {
        return static_cast<std::int64_t>(object.as<double>());
    }
    return std::nullopt;
}

PlotChannelDescriptor makePlotChannelDescriptor(const sol::table& channelTable, std::size_t index)
{
    PlotChannelDescriptor descriptor{};
    descriptor.label = luaStringField(channelTable, "label").value_or("CH" + std::to_string(index));
    descriptor.unit = luaStringField(channelTable, "unit").value_or("");
    descriptor.ratio = finiteOrDefault(luaNumberField(channelTable, "ratio").value_or(1.0), 1.0);
    descriptor.scale = finiteOrDefault(luaNumberField(channelTable, "scale").value_or(1.0), 1.0);
    descriptor.offset = finiteOrDefault(luaNumberField(channelTable, "offset").value_or(0.0), 0.0);
    return descriptor;
}

bool applyPlotChannelColor(PlotChannelDescriptor& descriptor,
                           const sol::table& channelTable,
                           std::size_t index,
                           std::string& error)
{
    const auto colorText = luaStringField(channelTable, "color");
    if (!colorText.has_value()) {
        return true;
    }

    descriptor.color = parseColorText(*colorText);
    if (descriptor.color.has_value()) {
        return true;
    }

    error = "plot.setup.channels[" + std::to_string(index) + "].color 必须是 #RRGGBB 或 #RRGGBBAA";
    return false;
}

bool applyPlotChannelLineWidth(PlotChannelDescriptor& descriptor,
                               const sol::table& channelTable,
                               std::size_t index,
                               std::string& error)
{
    const sol::object lineWidthObject = channelTable["line_width"];
    if (!lineWidthObject.valid() || lineWidthObject.get_type() == sol::type::lua_nil) {
        return true;
    }

    std::optional<double> lineWidth;
    if (lineWidthObject.is<double>()) {
        lineWidth = lineWidthObject.as<double>();
    } else if (lineWidthObject.is<int>()) {
        lineWidth = static_cast<double>(lineWidthObject.as<int>());
    }
    if (!lineWidth.has_value() || !std::isfinite(*lineWidth)) {
        error = "plot.setup.channels[" + std::to_string(index) + "].line_width 必须是有限数字";
        return false;
    }

    descriptor.lineWidth = plot::sanitizeChannelLineWidth(*lineWidth);
    return true;
}

std::optional<std::uint64_t> luaUnsignedIntegerValue(const sol::object& object)
{
    if (!object.valid() || object.get_type() == sol::type::lua_nil) {
        return std::nullopt;
    }
    if (object.is<int>()) {
        const int value = object.as<int>();
        if (value < 0) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(value);
    }
    if (!object.is<double>()) {
        return std::nullopt;
    }
    const double value = object.as<double>();
    if (!std::isfinite(value) || value < 0.0 || std::trunc(value) != value ||
        value > static_cast<double>((std::numeric_limits<std::uint64_t>::max)())) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(value);
}

std::optional<double> luaFiniteNumberValue(const sol::object& object)
{
    if (!object.valid() || object.get_type() == sol::type::lua_nil) {
        return std::nullopt;
    }
    std::optional<double> value;
    if (object.is<double>()) {
        value = object.as<double>();
    } else if (object.is<int>()) {
        value = static_cast<double>(object.as<int>());
    }
    if (!value.has_value() || !std::isfinite(*value)) {
        return std::nullopt;
    }
    return value;
}

bool applyPlotBitDisplayUnsignedField(const sol::table& bitTable,
                                      const char* fieldName,
                                      std::size_t channelIndex,
                                      std::uint64_t minValue,
                                      std::uint64_t maxValue,
                                      std::size_t& target,
                                      std::string& error)
{
    const sol::object object = bitTable[fieldName];
    if (!object.valid() || object.get_type() == sol::type::lua_nil) {
        return true;
    }
    const auto parsed = luaUnsignedIntegerValue(object);
    if (!parsed.has_value() || *parsed < minValue || *parsed > maxValue) {
        error = "plot.setup.channels[" + std::to_string(channelIndex) + "].bit_display." + fieldName + " 必须是 " +
                std::to_string(minValue) + ".." + std::to_string(maxValue) + " 的整数";
        return false;
    }
    target = static_cast<std::size_t>(*parsed);
    return true;
}

bool applyPlotChannelBitDisplay(PlotChannelDescriptor& descriptor,
                                const sol::table& channelTable,
                                std::size_t index,
                                std::string& error)
{
    const sol::object bitDisplayObject = channelTable["bit_display"];
    if (!bitDisplayObject.valid() || bitDisplayObject.get_type() == sol::type::lua_nil) {
        return true;
    }

    plot::BitDisplaySpec spec{};
    if (bitDisplayObject.is<bool>()) {
        spec.enabled = bitDisplayObject.as<bool>();
        descriptor.bitDisplay = plot::sanitizeBitDisplaySpec(spec);
        return true;
    }
    if (!bitDisplayObject.is<sol::table>()) {
        error = "plot.setup.channels[" + std::to_string(index) + "].bit_display 必须是 boolean 或 table";
        return false;
    }

    const sol::table bitTable = bitDisplayObject.as<sol::table>();
    spec.enabled = true;
    const sol::object enabledObject = bitTable["enabled"];
    if (enabledObject.valid() && enabledObject.get_type() != sol::type::lua_nil) {
        if (!enabledObject.is<bool>()) {
            error = "plot.setup.channels[" + std::to_string(index) + "].bit_display.enabled 必须是 boolean";
            return false;
        }
        spec.enabled = enabledObject.as<bool>();
    }
    if (!applyPlotBitDisplayUnsignedField(
            bitTable, "first_bit", index, 0, plot::kMaxBitDisplayCount - 1, spec.firstBit, error)) {
        return false;
    }
    if (!applyPlotBitDisplayUnsignedField(
            bitTable, "bit_count", index, 1, plot::kMaxBitDisplayCount, spec.bitCount, error)) {
        return false;
    }
    if (spec.firstBit + spec.bitCount > plot::kMaxBitDisplayCount) {
        error = "plot.setup.channels[" + std::to_string(index) + "].bit_display.first_bit + bit_count 不允许超过 64";
        return false;
    }

    const sol::object yOffsetObject = bitTable["y_offset"];
    if (yOffsetObject.valid() && yOffsetObject.get_type() != sol::type::lua_nil) {
        const auto yOffset = luaFiniteNumberValue(yOffsetObject);
        if (!yOffset.has_value()) {
            error = "plot.setup.channels[" + std::to_string(index) + "].bit_display.y_offset 必须是有限数字";
            return false;
        }
        spec.yOffset = *yOffset;
    }

    descriptor.bitDisplay = plot::sanitizeBitDisplaySpec(spec);
    return true;
}

std::optional<PlotChannelDescriptor> parsePlotChannelDescriptor(const sol::object& channelObject,
                                                                std::size_t index,
                                                                std::string& error)
{
    if (!channelObject.is<sol::table>()) {
        error = "plot.setup.channels[" + std::to_string(index) + "] 必须是 table";
        return std::nullopt;
    }

    const sol::table channelTable = channelObject.as<sol::table>();
    auto descriptor = makePlotChannelDescriptor(channelTable, index);
    if (!applyPlotChannelColor(descriptor, channelTable, index, error)) {
        return std::nullopt;
    }
    if (!applyPlotChannelLineWidth(descriptor, channelTable, index, error)) {
        return std::nullopt;
    }
    if (!applyPlotChannelBitDisplay(descriptor, channelTable, index, error)) {
        return std::nullopt;
    }
    return descriptor;
}

std::optional<std::vector<PlotChannelDescriptor>> parsePlotSetupChannels(const sol::table& table, std::string& error)
{
    const sol::object channelsObject = table["channels"];
    if (!channelsObject.is<sol::table>()) {
        error = "plot.setup.channels 必须是 table";
        return std::nullopt;
    }

    std::vector<PlotChannelDescriptor> channels;
    const sol::table channelsTable = channelsObject.as<sol::table>();
    channels.reserve(channelsTable.size());
    for (std::size_t index = 1; index <= channelsTable.size(); ++index) {
        auto descriptor = parsePlotChannelDescriptor(channelsTable[index], index, error);
        if (!descriptor.has_value()) {
            return std::nullopt;
        }
        channels.push_back(std::move(*descriptor));
    }
    if (channels.empty()) {
        error = "plot.setup.channels 不能为空";
        return std::nullopt;
    }
    return channels;
}

plot::ViewConfig parsePlotSetupViewConfig(const sol::table& table)
{
    plot::ViewConfig view{};
    view.timeScale = luaNumberField(table, "time_scale").value_or(1.0);
    view.timeUnit = luaStringField(table, "time_unit").value_or("s");
    view.verticalMin = luaNumberField(table, "vertical_min").value_or(-1.0);
    view.verticalMax = luaNumberField(table, "vertical_max").value_or(1.0);
    view.verticalUnit = luaStringField(table, "vertical_unit").value_or("V");
    const double historyLimit = luaNumberField(table, "history_limit").value_or(0.0);
    view.historyLimit = historyLimit <= 0.0 ? 0U : static_cast<std::size_t>(historyLimit);
    return view;
}

std::optional<PlotSetup> parsePlotSetup(const sol::object& object, std::string& error)
{
    if (!object.is<sol::table>()) {
        error = "plot.setup 参数必须是 table";
        return std::nullopt;
    }
    const sol::table table = object.as<sol::table>();
    auto channels = parsePlotSetupChannels(table, error);
    if (!channels.has_value()) {
        return std::nullopt;
    }

    // 顶层解析只负责编排字段，channel 与 view 的细节由 helper 保持内聚。
    PlotSetup setup{};
    setup.source = luaStringField(table, "source").value_or("");
    setup.resetHistory = luaBoolField(table, "reset_history").value_or(false);
    setup.channels = std::move(*channels);
    setup.view = parsePlotSetupViewConfig(table);
    return setup;
}

std::optional<plot::WaveSample> parseExpandedPlotSample(const sol::table& sampleTable,
                                                        std::size_t index,
                                                        std::string& error)
{
    const auto time = luaNumberField(sampleTable, "t");
    const auto value = luaNumberField(sampleTable, "y");
    if (!time.has_value() || !value.has_value()) {
        error = "plot.push.samples[" + std::to_string(index) + "] 必须包含数字字段 t / y";
        return std::nullopt;
    }
    return plot::WaveSample{.time = *time, .value = *value};
}

std::optional<std::vector<plot::WaveSample>> parseExpandedPlotSamples(const sol::table& samplesTable,
                                                                      std::string& error)
{
    std::vector<plot::WaveSample> samples;
    samples.reserve(samplesTable.size());
    for (std::size_t index = 1; index <= samplesTable.size(); ++index) {
        const sol::object sampleObject = samplesTable[index];
        if (!sampleObject.is<sol::table>()) {
            error = "plot.push.samples[" + std::to_string(index) + "] 必须是 table";
            return std::nullopt;
        }

        const auto sample = parseExpandedPlotSample(sampleObject.as<sol::table>(), index, error);
        if (!sample.has_value()) {
            return std::nullopt;
        }
        samples.push_back(*sample);
    }
    return samples;
}

std::optional<plot::WaveSample> parseCompactPlotSample(
    const sol::object& valueObject, double t0, double dt, std::size_t index, std::string& error)
{
    if (!valueObject.is<double>() && !valueObject.is<int>()) {
        error = "plot.push.values[" + std::to_string(index) + "] 必须是 number";
        return std::nullopt;
    }

    const double time = t0 + dt * static_cast<double>(index - 1);
    return plot::WaveSample{.time = time, .value = valueObject.as<double>()};
}

std::optional<std::vector<plot::WaveSample>> parseCompactPlotSamples(const sol::table& table, std::string& error)
{
    const sol::object valuesObject = table["values"];
    if (!valuesObject.is<sol::table>()) {
        error = "plot.push.samples 或 compact plot.push.values 必须是 table";
        return std::nullopt;
    }

    const auto t0 = luaNumberField(table, "t0");
    const auto dt = luaNumberField(table, "dt");
    if (!t0.has_value() || !dt.has_value()) {
        error = "compact plot.push 必须包含数字字段 t0 / dt";
        return std::nullopt;
    }

    std::vector<plot::WaveSample> samples;
    const sol::table valuesTable = valuesObject.as<sol::table>();
    samples.reserve(valuesTable.size());
    for (std::size_t index = 1; index <= valuesTable.size(); ++index) {
        const auto sample = parseCompactPlotSample(valuesTable[index], *t0, *dt, index, error);
        if (!sample.has_value()) {
            return std::nullopt;
        }
        samples.push_back(*sample);
    }
    return samples;
}

std::optional<std::vector<plot::WaveSample>> parsePlotAppendSamples(const sol::table& table, std::string& error)
{
    // Lua API 同时支持逐点 samples 与紧凑 values，两条路径在这里统一成 WaveSample 列表。
    const sol::object samplesObject = table["samples"];
    if (samplesObject.is<sol::table>()) {
        return parseExpandedPlotSamples(samplesObject.as<sol::table>(), error);
    }
    return parseCompactPlotSamples(table, error);
}

std::optional<plot::WaveAppendRequest> parsePlotAppend(const sol::object& object, std::string& error)
{
    if (!object.is<sol::table>()) {
        error = "plot.push 参数必须是 table";
        return std::nullopt;
    }
    const sol::table table = object.as<sol::table>();
    auto samples = parsePlotAppendSamples(table, error);
    if (!samples.has_value()) {
        return std::nullopt;
    }
    if (samples->empty()) {
        error = "plot.push 采样不能为空";
        return std::nullopt;
    }

    plot::WaveAppendRequest request{};
    request.source = luaStringField(table, "source").value_or("");
    request.samples = std::move(*samples);
    return request;
}

const ControlDescriptor* findControlDescriptor(const std::vector<ControlDescriptor>& controls, const std::string& id)
{
    for (const auto& control : controls) {
        if (control.id == id) {
            return &control;
        }
    }
    return nullptr;
}

namespace script_host_lua {

    std::string serializeLuaObject(const sol::object& object, int depth)
    {
        return protoscope::scripting::serializeLuaObject(object, depth);
    }

    const ControlDescriptor* findControlDescriptor(const std::vector<ControlDescriptor>& controls,
                                                   const std::string& id)
    {
        return protoscope::scripting::findControlDescriptor(controls, id);
    }

    std::optional<ControlValue> controlValueFromLua(const ControlDescriptor& descriptor,
                                                    const sol::object& object,
                                                    std::string& error)
    {
        return protoscope::scripting::controlValueFromLua(descriptor, object, error);
    }

    sol::object controlValueToLua(sol::state_view lua, const ControlDescriptor* descriptor, const ControlValue& value)
    {
        return protoscope::scripting::controlValueToLua(lua, descriptor, value);
    }

    std::optional<std::vector<std::uint8_t>> bytesFromLuaObject(const sol::object& object, std::string& error)
    {
        return protoscope::scripting::bytesFromLuaObject(object, error);
    }

    std::optional<PlotSetup> parsePlotSetup(const sol::object& object, std::string& error)
    {
        return protoscope::scripting::parsePlotSetup(object, error);
    }

    std::optional<plot::WaveAppendRequest> parsePlotAppend(const sol::object& object, std::string& error)
    {
        return protoscope::scripting::parsePlotAppend(object, error);
    }

} // namespace script_host_lua

ScriptHost::ScriptHost() : runtime_(std::make_unique<Runtime>()) {}

ScriptHost::~ScriptHost() = default;
ScriptHost::ScriptHost(ScriptHost&&) noexcept = default;
ScriptHost& ScriptHost::operator=(ScriptHost&&) noexcept = default;

void ScriptHost::setFileIoConfig(FileIoConfig config)
{
    fileIoConfig_ = std::move(config);
}

std::optional<StreamBufferDefinition> ScriptHost::streamBufferDefinition() const
{
    if (!runtime_ || !runtime_->stream) {
        return std::nullopt;
    }
    return runtime_->stream->parser.bufferDefinition();
}

std::vector<StreamFrameDefinition> ScriptHost::streamFrameDefinitions() const
{
    if (!runtime_ || !runtime_->stream) {
        return {};
    }
    return runtime_->stream->parser.frameDefinitions();
}

bool ScriptHost::setStreamRuntimeProfile(const sol::object& profileObject, std::string& error)
{
    if (!runtime_ || !runtime_->stream) {
        error = "当前协议未启用 stream()";
        return false;
    }
    if (!profileObject.valid() || !profileObject.is<sol::table>()) {
        error = "stream profile 必须是 table";
        return false;
    }
    const auto table = profileObject.as<sol::table>();
    const auto frameName = luaStringField(table, "frame");
    const auto lengthValue = luaIntegerValue(table["length"]);
    if (!frameName.has_value() || frameName->empty()) {
        error = "stream profile.frame 不能为空";
        return false;
    }
    if (!lengthValue.has_value() || *lengthValue <= 0) {
        error = "stream profile.length 必须是正整数";
        return false;
    }

    StreamRuntimeProfile profile;
    profile.length = static_cast<std::size_t>(*lengthValue);
    const sol::object channelMapObject = table["channel_map"];
    if (channelMapObject.valid() && channelMapObject.get_type() != sol::type::lua_nil) {
        if (!channelMapObject.is<sol::table>()) {
            error = "stream profile.channel_map 必须是数组";
            return false;
        }
        const auto channelMapTable = channelMapObject.as<sol::table>();
        profile.channelMap.reserve(channelMapTable.size());
        for (std::size_t index = 1; index <= channelMapTable.size(); ++index) {
            const auto value = luaIntegerValue(channelMapTable[index]);
            if (!value.has_value() || *value <= 0) {
                error = "stream profile.channel_map 必须是从 1 开始的正整数数组";
                return false;
            }
            profile.channelMap.push_back(static_cast<std::size_t>(*value - 1));
        }
    }

    if (!runtime_->stream->parser.setRuntimeProfile(*frameName, profile, error)) {
        return false;
    }
    runtime_->streamRuntimeProfiles.insert_or_assign(*frameName, std::move(profile));
    streamRuntimeProfileEvents_.push_back(StreamRuntimeProfileEvent{
        .cleared = false,
        .frameName = *frameName,
        .length = runtime_->streamRuntimeProfiles[*frameName].length,
        .channelMap = runtime_->streamRuntimeProfiles[*frameName].channelMap,
    });
    return true;
}

bool ScriptHost::clearStreamRuntimeProfile(const sol::object& frameNameObject, std::string& error)
{
    if (!runtime_ || !runtime_->stream) {
        error = "当前协议未启用 stream()";
        return false;
    }
    std::optional<std::string> frameName;
    if (frameNameObject.valid() && frameNameObject.get_type() != sol::type::lua_nil) {
        if (!frameNameObject.is<std::string>()) {
            error = "stream.clear_profile(frame) 仅接受字符串或 nil";
            return false;
        }
        frameName = frameNameObject.as<std::string>();
    }
    if (!runtime_->stream->parser.clearRuntimeProfile(frameName, error)) {
        return false;
    }
    if (frameName.has_value()) {
        runtime_->streamRuntimeProfiles.erase(*frameName);
        streamRuntimeProfileEvents_.push_back(StreamRuntimeProfileEvent{
            .cleared = true,
            .frameName = *frameName,
            .length = 0,
            .channelMap = {},
        });
    } else {
        runtime_->streamRuntimeProfiles.clear();
        streamRuntimeProfileEvents_.push_back(StreamRuntimeProfileEvent{
            .cleared = true,
            .frameName = {},
            .length = 0,
            .channelMap = {},
        });
    }
    return true;
}

bool ScriptHost::applyStreamRuntimeProfileEvent(const StreamRuntimeProfileEvent& event, std::string& error)
{
    if (!runtime_ || !runtime_->stream) {
        error = "当前协议未启用 stream()";
        return false;
    }
    if (event.cleared) {
        const std::optional<std::string> frameName =
            event.frameName.empty() ? std::nullopt : std::optional<std::string>{event.frameName};
        if (!runtime_->stream->parser.clearRuntimeProfile(frameName, error)) {
            return false;
        }
        if (frameName.has_value()) {
            runtime_->streamRuntimeProfiles.erase(*frameName);
        } else {
            runtime_->streamRuntimeProfiles.clear();
        }
        streamRuntimeProfileEvents_.push_back(event);
        return true;
    }
    if (event.frameName.empty() || event.length == 0U) {
        error = "stream profile.frame 和 length 不能为空";
        return false;
    }

    StreamRuntimeProfile profile{
        .length = event.length,
        .channelMap = event.channelMap,
    };
    if (!runtime_->stream->parser.setRuntimeProfile(event.frameName, profile, error)) {
        return false;
    }
    runtime_->streamRuntimeProfiles.insert_or_assign(event.frameName, std::move(profile));
    streamRuntimeProfileEvents_.push_back(event);
    return true;
}

void ScriptHost::clearAllStreamRuntimeProfiles()
{
    if (!runtime_ || !runtime_->stream) {
        return;
    }
    runtime_->stream->parser.clearRuntimeProfiles();
    runtime_->streamRuntimeProfiles.clear();
}

std::optional<StreamParseBatch> ScriptHost::lastStreamParseBatch() const
{
    if (!runtime_ || !runtime_->stream) {
        return std::nullopt;
    }
    return runtime_->stream->lastBatch;
}

void ScriptHost::resetRuntime()
{
    scriptLoaded_ = false;
    lastError_.clear();
    docks_.clear();
    controls_.clear();
    controlValues_.clear();
    events_.clear();
    logs_.clear();
    txRequests_.clear();
    timers_.clear();
    requestDoneResults_.clear();
    statusUpdates_.clear();
    dialogRequests_.clear();
    fileDialogRequests_.clear();
    fileSendJobs_.clear();
    fileHandles_.clear();
    dialogAuthorizedPaths_.clear();
    activeConnection_.reset();
    requestAwaitingCompletion_ = false;
    runtime_ = std::make_unique<Runtime>();
}

void ScriptHost::onTransportOpen(const transport::TransportOpenEvent& event)
{
    activeConnection_ = event.context;
    if (runtime_->stream) {
        // runtime_profile 是脚本显式设置的解析契约，应持续生效直到 clear_profile()。
        runtime_->stream->parser.reset();
    }
    callbackOnOpen(ScriptHostContext{event.context});
}

void ScriptHost::onTransportClose(const transport::TransportCloseEvent& event)
{
    callbackOnClose(ScriptHostContext{event.context});
    if (activeConnection_.has_value() && activeConnection_->connectionId == event.context.connectionId) {
        activeConnection_.reset();
    }
    if (runtime_->stream) {
        runtime_->stream->parser.reset();
    }
}

void ScriptHost::onTransportError(const transport::TransportErrorEvent& event)
{
    callbackOnError(ScriptHostContext{event.context}, event.message);
}

void ScriptHost::beginTransportBytesEvent(const transport::TransportBytesEvent& event)
{
    lastTransportStats_ = ScriptHostTransportStats{
        .bytes = event.bytes.size(),
        .streamMode = runtime_ && runtime_->stream,
        .lastErrorSummary{},
    };
    if (event.context.readyForIo) {
        activeConnection_ = event.context;
    }
}

StreamParseBatch ScriptHost::parseTransportStreamBytes(const std::vector<std::uint8_t>& bytes,
                                                       std::chrono::steady_clock::time_point& parserFinishedAt)
{
    const auto parserStartedAt = std::chrono::steady_clock::now();
    const StreamParseOptions parseOptions{
        .includeFrameRaw = runtime_->stream->includeRawFrames || !runtime_->stream->lowOverhead,
    };
    const auto batch = runtime_->stream->parser.pushBytes(bytes, parseOptions);
    runtime_->stream->lastBatch = makeStreamParseBatchSnapshot(batch, runtime_->stream->lowOverhead);
    parserFinishedAt = std::chrono::steady_clock::now();
    lastTransportStats_.streamFrames = batch.frames.size();
    lastTransportStats_.streamErrors = batch.errors.size();
    lastTransportStats_.parserMs = elapsedMilliseconds(parserStartedAt, parserFinishedAt);
    return batch;
}

void ScriptHost::dispatchStreamParseErrors(const transport::ConnectionContext& context, const StreamParseBatch& batch)
{
    for (const auto& error : batch.errors) {
        callbackOnStreamError(ScriptHostContext{context}, error);
    }
}

void ScriptHost::updateStreamParseErrorSummary(const StreamParseBatch& batch)
{
    if (batch.errors.empty()) {
        lastTransportStats_.lastErrorSummary.clear();
        return;
    }

    std::unordered_map<StreamParseErrorCode, std::size_t> errorCounts;
    std::string crcFrameNames;
    std::size_t overflowDroppedBytes = 0;
    for (const auto& error : batch.errors) {
        errorCounts[error.code]++;
        if (error.code == StreamParseErrorCode::CrcMismatch && error.frameName.has_value()) {
            if (!crcFrameNames.empty()) {
                crcFrameNames += ", ";
            }
            crcFrameNames += *error.frameName;
        }
        if (error.code == StreamParseErrorCode::Overflow) {
            overflowDroppedBytes += error.droppedBytes;
        }
    }

    std::string summary;
    for (const auto& [code, count] : errorCounts) {
        if (!summary.empty()) {
            summary += ", ";
        }
        summary += std::string(streamParseErrorCodeName(code)) + " × " + std::to_string(count);
        if (code == StreamParseErrorCode::Overflow) {
            summary += " (dropped=" + std::to_string(overflowDroppedBytes) +
                       " bytes, capacity=" + std::to_string(batch.bufferCapacity) + " bytes)";
        }
    }
    protoLog("warn", "stream parse errors: " + summary);
    if (!crcFrameNames.empty()) {
        protoLog("warn", "  crc_mismatch frames: " + crcFrameNames);
    }
    lastTransportStats_.lastErrorSummary = std::move(summary);
}

void ScriptHost::applyStreamValueTargets(const std::vector<StreamParsedFrame>& frames)
{
    if (!runtime_ || !runtime_->stream || runtime_->stream->valueTargetsByFrame.empty()) {
        return;
    }

    for (const auto& frame : frames) {
        const auto targetsIter = runtime_->stream->valueTargetsByFrame.find(frame.name);
        if (targetsIter == runtime_->stream->valueTargetsByFrame.end()) {
            continue;
        }
        for (const auto& target : targetsIter->second) {
            const auto* descriptor = findControlDescriptor(controls_, target.controlId);
            if (descriptor == nullptr || descriptor->type != ControlType::ValueTable) {
                continue;
            }

            std::optional<std::uint32_t> startId = target.startId;
            if (!startId.has_value() && target.startField.has_value()) {
                const auto startIter = frame.fields.find(*target.startField);
                if (startIter == frame.fields.end()) {
                    continue;
                }
                startId = streamFieldU32(startIter->second);
            }
            if (!startId.has_value()) {
                continue;
            }

            const auto valuesIter = frame.fields.find(target.valuesField);
            if (valuesIter == frame.fields.end()) {
                continue;
            }

            auto current = defaultValueTableFor(*descriptor);
            if (const auto currentIter = controlValues_.find(target.controlId); currentIter != controlValues_.end()) {
                if (const auto* tableValue = std::get_if<ValueTableValue>(&currentIter->second);
                    tableValue != nullptr) {
                    current = *tableValue;
                }
            }

            auto patch = defaultValueTableFor(*descriptor);
            if (!applyStreamValueTargetValues(*descriptor, patch, *startId, valuesIter->second)) {
                continue;
            }
            mergeValueTableValue(*descriptor, current, patch);
            controlValues_[target.controlId] = std::move(current);
        }
    }
}

void ScriptHost::dispatchStreamFrames(const transport::ConnectionContext& context,
                                      const std::vector<StreamParsedFrame>& frames)
{
    // 核心流程：同一 parser 链顺序产出完整帧；若脚本支持 on_batch，则只批量调用一次，避免重复 on_frame。
    if (!frames.empty() && !callbackOnStreamBatch(ScriptHostContext{context}, frames)) {
        for (const auto& frame : frames) {
            callbackOnStreamFrame(ScriptHostContext{context}, frame);
        }
    }
}

void ScriptHost::handleStreamTransportBytes(const transport::TransportBytesEvent& event,
                                            std::chrono::steady_clock::time_point startedAt)
{
    std::chrono::steady_clock::time_point parserFinishedAt;
    const auto batch = parseTransportStreamBytes(event.bytes, parserFinishedAt);
    const auto callbackStartedAt = parserFinishedAt;
    dispatchStreamParseErrors(event.context, batch);
    updateStreamParseErrorSummary(batch);
    applyStreamValueTargets(batch.frames);
    dispatchStreamFrames(event.context, batch.frames);
    const auto finishedAt = std::chrono::steady_clock::now();
    lastTransportStats_.callbackMs = elapsedMilliseconds(callbackStartedAt, finishedAt);
    lastTransportStats_.totalMs = elapsedMilliseconds(startedAt, finishedAt);
}

void ScriptHost::handleRawTransportBytes(const transport::TransportBytesEvent& event,
                                         std::chrono::steady_clock::time_point startedAt)
{
    const auto callbackStartedAt = std::chrono::steady_clock::now();
    callbackOnBytes(ScriptHostContext{event.context}, event.bytes);
    const auto finishedAt = std::chrono::steady_clock::now();
    lastTransportStats_.callbackMs = elapsedMilliseconds(callbackStartedAt, finishedAt);
    lastTransportStats_.totalMs = elapsedMilliseconds(startedAt, finishedAt);
}

void ScriptHost::onTransportBytes(const transport::TransportBytesEvent& event)
{
    const auto startedAt = std::chrono::steady_clock::now();
    beginTransportBytesEvent(event);
    if (runtime_->stream) {
        handleStreamTransportBytes(event, startedAt);
        return;
    }
    handleRawTransportBytes(event, startedAt);
}

void ScriptHost::onControl(const transport::ConnectionContext& ctx, const std::string& id, const ControlValue& value)
{
    const auto* descriptor = findControlDescriptor(controls_, id);
    if (descriptor == nullptr) {
        return;
    }
    if (descriptor->type == ControlType::ValueTable) {
        auto current = defaultValueTableFor(*descriptor);
        if (const auto iter = controlValues_.find(id); iter != controlValues_.end()) {
            if (const auto* tableValue = std::get_if<ValueTableValue>(&iter->second); tableValue != nullptr) {
                current = *tableValue;
            }
        }
        if (const auto* patch = std::get_if<ValueTableValue>(&value); patch != nullptr) {
            mergeValueTableValue(*descriptor, current, *patch);
        }
        controlValues_[id] = std::move(current);
        callbackOnControl(ScriptHostContext{ctx}, id, value);
        return;
    }
    controlValues_[id] = value;
    callbackOnControl(ScriptHostContext{ctx}, id, value);
}

bool ScriptHost::requestOscilloscopeToggle(const transport::ConnectionContext& ctx,
                                           bool currentRunning,
                                           bool targetRunning)
{
    const auto updateCountBeforeCallback = oscilloscopeRunningUpdates_.size();
    const bool accepted = callbackOnOscilloscopeToggle(ScriptHostContext{ctx}, currentRunning, targetRunning);
    if (accepted && oscilloscopeRunningUpdates_.size() == updateCountBeforeCallback) {
        // 核心流程：旧脚本只返回 true 时仍按目标状态同步；显式 set_running 优先。
        protoOscilloscopeSetRunning(targetRunning);
    }
    return accepted;
}

bool ScriptHost::setControlValue(const std::string& id, const ControlValue& value)
{
    const auto* descriptor = findControlDescriptor(controls_, id);
    if (descriptor == nullptr) {
        return false;
    }
    if (descriptor->type == ControlType::ValueTable) {
        auto next = defaultValueTableFor(*descriptor);
        if (const auto* tableValue = std::get_if<ValueTableValue>(&value); tableValue != nullptr) {
            mergeValueTableValue(*descriptor, next, *tableValue);
        }
        controlValues_[id] = std::move(next);
        return true;
    }
    controlValues_[id] = value;
    return true;
}

void ScriptHost::tick(std::uint64_t currentMs)
{
    std::vector<std::string> dueTimers;
    dueTimers.reserve(timers_.size());
    for (const auto& [name, timer] : timers_) {
        if (timer.active && currentMs >= timer.dueAtMs) {
            dueTimers.push_back(name);
        }
    }

    for (const auto& name : dueTimers) {
        auto iter = timers_.find(name);
        if (iter != timers_.end()) {
            iter->second.active = false;
        }
        if (activeConnection_.has_value()) {
            callbackOnTimer(ScriptHostContext{*activeConnection_}, name);
        } else {
            transport::ConnectionContext context;
            context.kind = transport::TransportKind::TcpClient;
            context.endpoint = "detached";
            context.connectionId = 0;
            context.timestampMs = currentMs;
            context.readyForIo = false;
            callbackOnTimer(ScriptHostContext{context}, name);
        }
    }
}

std::vector<ControlDescriptor> ScriptHost::controlsSnapshot() const
{
    return controls_;
}

std::vector<ControlSnapshot> ScriptHost::controlStatesSnapshot() const
{
    std::vector<ControlSnapshot> snapshot;
    snapshot.reserve(controls_.size());
    for (const auto& control : controls_) {
        const auto iter = controlValues_.find(control.id);
        snapshot.push_back(ControlSnapshot{
            .descriptor = control,
            .value = iter == controlValues_.end() ? defaultValueFor(control) : iter->second,
        });
    }
    return snapshot;
}

std::vector<DockDescriptor> ScriptHost::dockDescriptorsSnapshot() const
{
    return docks_;
}

std::vector<DockSnapshot> ScriptHost::dockSnapshots() const
{
    std::vector<DockSnapshot> docks;
    docks.reserve(docks_.size());
    for (const auto& dock : docks_) {
        DockSnapshot snapshot;
        snapshot.descriptor = dock;
        snapshot.controls.reserve(dock.controls.size());
        for (const auto& control : dock.controls) {
            const auto iter = controlValues_.find(control.id);
            snapshot.controls.push_back(ControlSnapshot{
                .descriptor = control,
                .value = iter == controlValues_.end() ? defaultValueFor(control) : iter->second,
            });
        }
        docks.push_back(std::move(snapshot));
    }
    return docks;
}

std::vector<ScriptEvent> ScriptHost::drainEvents()
{
    auto drained = std::move(events_);
    events_.clear();
    return drained;
}

std::vector<ScriptLog> ScriptHost::drainLogs()
{
    auto drained = std::move(logs_);
    logs_.clear();
    return drained;
}

const ScriptHostTransportStats& ScriptHost::lastTransportStats() const
{
    return lastTransportStats_;
}

std::vector<TxRequest> ScriptHost::drainTxRequests()
{
    auto drained = std::move(txRequests_);
    txRequests_.clear();
    return drained;
}

std::vector<transport::ConnectionContext> ScriptHost::drainRequestGuardResets()
{
    auto drained = std::move(requestGuardResets_);
    requestGuardResets_.clear();
    return drained;
}

std::vector<PlotSetup> ScriptHost::drainPlotSetups()
{
    auto drained = std::move(plotSetups_);
    plotSetups_.clear();
    return drained;
}

std::vector<std::pair<std::size_t, plot::WaveAppendRequest>> ScriptHost::drainPlotAppends()
{
    auto drained = std::move(plotAppends_);
    plotAppends_.clear();
    return drained;
}

std::vector<std::pair<std::size_t, plot::WaveAppendRequest>> ScriptHost::drainPlotAppends(const std::size_t maxRequests)
{
    if (plotAppends_.empty() || maxRequests == 0U) {
        return {};
    }

    auto makeKey = [](const std::pair<std::size_t, plot::WaveAppendRequest>& append) {
        std::string key = std::to_string(append.first);
        key.push_back('\x1F');
        key.append(append.second.source);
        return key;
    };

    auto count = (std::min)(maxRequests, plotAppends_.size());
    for (;;) {
        std::unordered_set<std::string> keys;
        keys.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            keys.insert(makeKey(plotAppends_[index]));
        }

        auto extendedCount = count;
        for (std::size_t index = count; index < plotAppends_.size(); ++index) {
            if (keys.contains(makeKey(plotAppends_[index]))) {
                extendedCount = index + 1U;
            }
        }
        if (extendedCount == count) {
            break;
        }
        count = extendedCount;
    }

    std::vector<std::pair<std::size_t, plot::WaveAppendRequest>> drained;
    drained.reserve(count);
    auto end = plotAppends_.begin();
    std::advance(end, static_cast<std::ptrdiff_t>(count));
    std::move(plotAppends_.begin(), end, std::back_inserter(drained));
    plotAppends_.erase(plotAppends_.begin(), end);
    return drained;
}

std::size_t ScriptHost::pendingPlotAppendCount() const
{
    return plotAppends_.size();
}

RealtimeOutputDiscardCounts ScriptHost::clearPendingRealtimeOutputs()
{
    const RealtimeOutputDiscardCounts counts{
        .events = events_.size(),
        .logs = logs_.size(),
        .plotAppends = plotAppends_.size(),
    };
    events_.clear();
    logs_.clear();
    plotAppends_.clear();
    return counts;
}

std::vector<RequestDoneResult> ScriptHost::drainRequestDoneResults()
{
    auto drained = std::move(requestDoneResults_);
    requestDoneResults_.clear();
    return drained;
}

std::vector<StatusUpdate> ScriptHost::drainStatusUpdates()
{
    auto drained = std::move(statusUpdates_);
    statusUpdates_.clear();
    return drained;
}

std::vector<OscilloscopeRunningUpdate> ScriptHost::drainOscilloscopeRunningUpdates()
{
    auto drained = std::move(oscilloscopeRunningUpdates_);
    oscilloscopeRunningUpdates_.clear();
    return drained;
}

std::vector<StreamRuntimeProfileEvent> ScriptHost::drainStreamRuntimeProfileEvents()
{
    auto drained = std::move(streamRuntimeProfileEvents_);
    streamRuntimeProfileEvents_.clear();
    return drained;
}

std::vector<DialogRequest> ScriptHost::drainDialogRequests()
{
    auto drained = std::move(dialogRequests_);
    dialogRequests_.clear();
    return drained;
}

std::vector<FileDialogRequest> ScriptHost::drainFileDialogRequests()
{
    auto drained = std::move(fileDialogRequests_);
    fileDialogRequests_.clear();
    return drained;
}

void ScriptHost::registerLuaApi(sol::state_view lua, sol::table& proto)
{
    ScriptHostQueues queues;
    ScriptHostContextInternal ctx{*this, queues, fileIoConfig_, activeConnection_, lua};
    std::array modules{
        makeCoreApiModule(*this),
        makeTxApiModule(*this),
        makeStatusApiModule(*this),
        makeUiApiModule(*this),
        makeFileApiModule(*this),
        makeOscilloscopeApiModule(*this),
        makePlotApiModule(*this),
        makeControlApiModule(*this),
        makeCodecApiModule(*this),
    };

    // 核心流程：宿主只编排模块顺序，具体 Lua wire format 由各 API 模块原样注册。
    for (auto& module : modules) {
        module->registerApi(ctx, proto);
    }
}

std::optional<std::uint64_t> ScriptHost::nextWakeupAtMs() const
{
    std::optional<std::uint64_t> nextWakeup;
    for (const auto& [_, timer] : timers_) {
        if (!timer.active) {
            continue;
        }
        if (!nextWakeup.has_value() || timer.dueAtMs < *nextWakeup) {
            nextWakeup = timer.dueAtMs;
        }
    }
    return nextWakeup;
}

const std::string& ScriptHost::scriptPath() const
{
    return scriptPath_;
}

const std::string& ScriptHost::protocolDirectory() const
{
    return protocolDirectory_;
}

const std::string& ScriptHost::lastError() const
{
    return lastError_;
}

void ScriptHost::onTxEvent(const transport::ConnectionContext& ctx, const TxEvent& event)
{
    const bool releaseFileChunk = event.state == TxEventState::Sent || event.state == TxEventState::Rejected ||
                                  event.state == TxEventState::Dropped || event.state == TxEventState::Canceled ||
                                  event.state == TxEventState::Timeout;
    if (event.fileJobId != 0 && releaseFileChunk) {
        const auto iter = fileSendJobs_.find(event.fileJobId);
        if (iter != fileSendJobs_.end()) {
            iter->second.inflight = iter->second.inflight == 0 ? 0 : iter->second.inflight - 1;
            pumpFileSendJob(event.fileJobId);
        }
    }
    callbackOnTx(ScriptHostContext{ctx}, event);
}

void ScriptHost::onDialogEvent(const transport::ConnectionContext& ctx, const DialogEvent& event)
{
    callbackOnDialog(ScriptHostContext{ctx}, event);
}

void ScriptHost::onFileDialogEvent(const transport::ConnectionContext& ctx, const FileDialogEvent& event)
{
    if (event.state == "selected" && !event.path.empty() && fileIoConfig_.allowDialogPaths) {
        std::error_code errorCode;
        auto path = std::filesystem::weakly_canonical(std::filesystem::absolute(event.path), errorCode);
        if (errorCode) {
            path = std::filesystem::absolute(event.path).lexically_normal();
        }
        dialogAuthorizedPaths_.push_back(AuthorizedPath{
            .path = path,
            .recursive = event.kind == FileDialogKind::OpenDir,
            .readable = event.kind != FileDialogKind::SaveFile,
            .writable = event.kind != FileDialogKind::OpenFile,
        });
    }
    callbackOnFileDialog(ScriptHostContext{ctx}, event);
}

void ScriptHost::setRequestAwaitingCompletion(bool active)
{
    requestAwaitingCompletion_ = active;
}

sol::state& ScriptHost::luaState()
{
    return runtime_->lua;
}

sol::state_view ScriptHost::luaView()
{
    return sol::state_view(runtime_->lua.lua_state());
}

const std::vector<ControlDescriptor>& ScriptHost::controlDescriptors() const
{
    return controls_;
}

const ControlValue* ScriptHost::findControlValue(const std::string& id) const
{
    const auto iter = controlValues_.find(id);
    return iter == controlValues_.end() ? nullptr : &iter->second;
}

void ScriptHost::updateControlValue(const std::string& id, ControlValue value)
{
    const auto* descriptor = findControlDescriptor(controls_, id);
    if (descriptor != nullptr && descriptor->type == ControlType::ValueTable) {
        auto current = defaultValueTableFor(*descriptor);
        if (const auto iter = controlValues_.find(id); iter != controlValues_.end()) {
            if (const auto* tableValue = std::get_if<ValueTableValue>(&iter->second); tableValue != nullptr) {
                current = *tableValue;
            }
        }
        if (const auto* patch = std::get_if<ValueTableValue>(&value); patch != nullptr) {
            mergeValueTableValue(*descriptor, current, *patch);
        }
        controlValues_[id] = std::move(current);
        return;
    }
    controlValues_[id] = std::move(value);
}

TxRequest ScriptHost::createTxRequest(TxRequestKind kind, std::vector<std::uint8_t> payload, bool guarded)
{
    TxRequest request{};
    request.id = nextTxRequestId();
    request.kind = kind;
    request.payload = std::move(payload);
    request.timeoutMs = 0;
    request.guarded = guarded;
    request.attempt = 1;
    request.maxAttempts = 1;
    request.createdAtMs = nowMs();
    applyTxRequestConnection(request);
    return request;
}

void ScriptHost::applyTxRequestConnection(TxRequest& request) const
{
    if (activeConnection_.has_value()) {
        request.connection = *activeConnection_;
    } else {
        request.connection.endpoint = "detached";
        request.connection.connectionId = 0;
        request.connection.timestampMs = request.createdAtMs;
        request.connection.readyForIo = false;
    }
}

bool ScriptHost::applyTxRequestOptions(
    TxRequest& request, const sol::object& opts, const std::string& apiName, std::string& error, bool guarded)
{
    if (opts.valid() && opts.get_type() != sol::type::lua_nil) {
        if (!opts.is<sol::table>()) {
            error = "opts 必须是 table";
            protoLog("error", apiName + " 调用失败: " + error);
            return false;
        }
        const sol::table options = opts.as<sol::table>();
        if (const auto timeoutMs = luaNumberField(options, "timeout_ms"); timeoutMs.has_value()) {
            request.timeoutMs = static_cast<std::uint64_t>(std::max(0.0, *timeoutMs));
        }
        request.tag = luaStringField(options, "tag").value_or("");
        if (guarded) {
            if (const auto maxAttempts = luaNumberField(options, "max_attempts"); maxAttempts.has_value()) {
                request.maxAttempts = static_cast<std::uint32_t>(std::max(1.0, *maxAttempts));
            }
        }
    }
    return true;
}

std::optional<TxRequest> ScriptHost::protoSendLike(
    TxRequestKind kind, const sol::object& payload, const sol::object& opts, std::string& error, bool guarded)
{
    const std::string apiName = txRequestApiName(kind, guarded);
    auto maybeBytes = bytesFromLuaObject(payload, error);
    if (!maybeBytes.has_value()) {
        protoLog("error", apiName + " 调用失败: " + error);
        return std::nullopt;
    }

    TxRequest request = createTxRequest(kind, std::move(*maybeBytes), guarded);
    if (!applyTxRequestOptions(request, opts, apiName, error, guarded)) {
        return std::nullopt;
    }

    txRequests_.push_back(request);
    return request;
}

void ScriptHost::protoResetRequestGuard()
{
    transport::ConnectionContext connection{};
    if (activeConnection_.has_value()) {
        connection = *activeConnection_;
    } else {
        connection.endpoint = "detached";
        connection.connectionId = 0;
        connection.timestampMs = nowMs();
        connection.readyForIo = false;
    }
    requestGuardResets_.push_back(std::move(connection));
}

bool ScriptHost::protoRequestDone(const sol::object& result, std::string& error)
{
    if (!requestAwaitingCompletion_) {
        error = "当前没有活动 request";
        protoLog("warn", "proto.request_done 被忽略: " + error);
        return false;
    }

    RequestDoneResult requestDone{};
    requestDone.timestampMs = nowMs();
    if (result.valid() && result.get_type() != sol::type::lua_nil) {
        if (!result.is<sol::table>()) {
            error = "result 必须是 table";
            protoLog("error", "proto.request_done 调用失败: " + error);
            return false;
        }
        const sol::table table = result.as<sol::table>();
        requestDone.ok = luaBoolField(table, "ok").value_or(true);
        requestDone.message = luaStringField(table, "message").value_or("");
    }

    requestDoneResults_.push_back(requestDone);
    return true;
}

void ScriptHost::protoStatusSet(const std::string& text, const sol::object& opts)
{
    StatusUpdate update{};
    update.text = text;
    update.level = "info";
    update.timestampMs = nowMs();
    if (opts.valid() && opts.get_type() != sol::type::lua_nil && opts.is<sol::table>()) {
        update.level = luaStringField(opts.as<sol::table>(), "level").value_or(update.level);
    }
    statusUpdates_.push_back(std::move(update));
}

void ScriptHost::protoStatusClear()
{
    StatusUpdate update{};
    update.text.clear();
    update.level = "info";
    update.clear = true;
    update.timestampMs = nowMs();
    statusUpdates_.push_back(std::move(update));
}

std::optional<DialogRequest> ScriptHost::protoDialog(DialogKind kind, const sol::object& opts, std::string& error)
{
    if (!opts.is<sol::table>()) {
        error = "opts 必须是 table";
        protoLog(
            "error",
            std::string(kind == DialogKind::Alert ? "proto.ui.alert" : "proto.ui.confirm") + " 调用失败: " + error);
        return std::nullopt;
    }

    const sol::table table = opts.as<sol::table>();
    const auto createdAtMs = nowMs();
    transport::ConnectionContext connection{};
    if (activeConnection_.has_value()) {
        connection = *activeConnection_;
    } else {
        connection.endpoint = "detached";
        connection.connectionId = 0;
        connection.timestampMs = createdAtMs;
        connection.readyForIo = false;
    }
    const auto window = luaDialogWindowOptions(table, error);
    if (!window.has_value()) {
        protoLog(
            "error",
            std::string(kind == DialogKind::Alert ? "proto.ui.alert" : "proto.ui.confirm") + " 调用失败: " + error);
        return std::nullopt;
    }
    const DialogRequest request{
        .id = nextDialogId(),
        .kind = kind,
        .connection = connection,
        .title = luaStringField(table, "title").value_or(""),
        .message = luaStringField(table, "message").value_or(""),
        .level = luaStringField(table, "level").value_or("info"),
        .dedupeKey = luaStringField(table, "dedupe_key").value_or(""),
        .window = *window,
        .createdAtMs = createdAtMs,
    };
    if (request.title.empty() || request.message.empty()) {
        error = "title 和 message 不能为空";
        protoLog(
            "error",
            std::string(kind == DialogKind::Alert ? "proto.ui.alert" : "proto.ui.confirm") + " 调用失败: " + error);
        return std::nullopt;
    }
    dialogRequests_.push_back(request);
    return request;
}

std::optional<FileDialogRequest> ScriptHost::protoFileDialog(FileDialogKind kind,
                                                             const sol::object& opts,
                                                             std::string& error)
{
    if (!fileIoConfig_.enabled) {
        error = "scripting.file_io 已禁用";
        return std::nullopt;
    }
    if (!fileIoConfig_.dialog.enabled) {
        error = "scripting.file_io.dialog 已禁用";
        return std::nullopt;
    }
    if (!opts.is<sol::table>()) {
        error = "opts 必须是 table";
        return std::nullopt;
    }

    const auto table = opts.as<sol::table>();
    const auto resolvedKind = resolveFileDialogKind(kind, table, error);
    if (!resolvedKind.has_value()) {
        return std::nullopt;
    }

    const auto createdAtMs = nowMs();
    auto filters = parseFileDialogFilters(table, error);
    if (!filters.has_value()) {
        return std::nullopt;
    }

    // 成员函数只补齐宿主状态，Lua 参数解析保持在无状态 helper 中。
    const FileDialogRequest request = makeFileDialogRequest(nextFileDialogId(),
                                                            *resolvedKind,
                                                            fileDialogConnectionContext(activeConnection_, createdAtMs),
                                                            table,
                                                            std::move(*filters),
                                                            createdAtMs);
    fileDialogRequests_.push_back(request);
    return request;
}

std::uint64_t ScriptHost::nextTxRequestId()
{
    return nextTxRequestId_++;
}

std::uint64_t ScriptHost::nextDialogId()
{
    return nextDialogId_++;
}

std::uint64_t ScriptHost::nextFileDialogId()
{
    return nextFileDialogId_++;
}

std::uint64_t ScriptHost::nextFileHandleId()
{
    return nextFileHandleId_++;
}

std::uint64_t ScriptHost::nextFileJobId()
{
    return nextFileJobId_++;
}

void ScriptHost::protoLog(const std::string& level, const std::string& message)
{
    logs_.push_back(ScriptLog{
        .level = level,
        .message = message,
        .timestampMs = nowMs(),
    });
}

void ScriptHost::protoEmit(const std::string& eventName, const std::string& payload)
{
    events_.push_back(ScriptEvent{
        .name = eventName,
        .payload = payload,
        .timestampMs = nowMs(),
    });
}

void ScriptHost::protoSetTimer(const std::string& name, std::uint64_t intervalMs)
{
    timers_[name] = TimerState{
        .name = name,
        .dueAtMs = nowMs() + intervalMs,
        .active = true,
    };
}

void ScriptHost::protoCancelTimer(const std::string& name)
{
    const auto iter = timers_.find(name);
    if (iter != timers_.end()) {
        iter->second.active = false;
    }
}

void ScriptHost::protoOscilloscopeSetRunning(bool running)
{
    oscilloscopeRunningUpdates_.push_back(OscilloscopeRunningUpdate{.running = running});
}

void ScriptHost::protoPlotSetup(const PlotSetup& setup)
{
    plotSetups_.push_back(setup);
}

void ScriptHost::protoPlotPush(std::size_t channelIndex, const plot::WaveAppendRequest& request)
{
    plotAppends_.push_back(std::make_pair(channelIndex, request));
}

std::string ScriptHost::valueToString(const ControlValue& value)
{
    return std::visit(
        [](const auto& current) {
            using ValueType = std::decay_t<decltype(current)>;
            if constexpr (std::is_same_v<ValueType, ElfSymbolValue>) {
                if (current.label.empty() || current.value.empty() || current.type.empty()) {
                    return std::string("nil");
                }
                return current.label + "=" + current.value + " (" + current.type + ")";
            } else if constexpr (std::is_same_v<ValueType, ValueTableValue>) {
                std::size_t setCount = 0;
                for (const auto& row : current.rows) {
                    if (row.set) {
                        ++setCount;
                    }
                }
                return "value_table rows=" + std::to_string(setCount);
            } else if constexpr (std::is_same_v<ValueType, TxSequenceValue>) {
                return "tx_sequence frames=" + std::to_string(current.frames.size()) +
                       " running=" + (current.running ? "true" : "false");
            } else {
                std::ostringstream builder;
                builder << current;
                return builder.str();
            }
        },
        value);
}

void ScriptHost::setLastError(std::string message)
{
    lastError_ = std::move(message);
}

} // namespace protoscope::scripting
