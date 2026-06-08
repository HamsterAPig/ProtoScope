#include "script_host_internal.hpp"

#include <unordered_set>
#include <utility>

namespace protoscope::scripting {

std::string streamFieldCountPath(const std::size_t frameIndex, const std::size_t fieldIndex)
{
    return "stream.frames[" + std::to_string(frameIndex + 1) + "].fields[" + std::to_string(fieldIndex + 1) + "].count";
}

std::string frameOnFrameCallbackKey(const std::string& frameName)
{
    return "stream.frame." + frameName + ".on_frame";
}

std::optional<StreamValueType> parseStreamValueType(const std::string& text)
{
    return streamValueTypeFromName(text);
}

bool streamValueTypeCanBeLength(StreamValueType type)
{
    return type != StreamValueType::Bytes && !streamValueTypeIsFloat(type);
}

std::optional<StreamLengthMeans> parseStreamLengthMeans(const std::string& text)
{
    if (text == "payload") {
        return StreamLengthMeans::Payload;
    }
    if (text == "frame") {
        return StreamLengthMeans::Frame;
    }
    return std::nullopt;
}

std::optional<StreamCrcType> parseStreamCrcType(const std::string& text)
{
    if (text == "crc16_modbus") {
        return StreamCrcType::Crc16Modbus;
    }
    if (text == "crc16_ccitt_false") {
        return StreamCrcType::Crc16CcittFalse;
    }
    if (text == "crc32_ieee") {
        return StreamCrcType::Crc32Ieee;
    }
    return std::nullopt;
}

std::optional<StreamCrcOrder> parseStreamCrcOrder(const std::string& text)
{
    if (text == "hi_lo") {
        return StreamCrcOrder::HiLo;
    }
    if (text == "lo_hi") {
        return StreamCrcOrder::LoHi;
    }
    return std::nullopt;
}

sol::object streamFieldValueToLua(sol::state_view lua, const StreamFieldValue& value)
{
    if (std::holds_alternative<std::int64_t>(value.value)) {
        return sol::make_object(lua, std::get<std::int64_t>(value.value));
    }
    if (std::holds_alternative<double>(value.value)) {
        return sol::make_object(lua, std::get<double>(value.value));
    }
    if (std::holds_alternative<std::vector<std::uint8_t>>(value.value)) {
        return sol::make_object(lua, makeBytesTable(lua, std::get<std::vector<std::uint8_t>>(value.value)));
    }
    if (std::holds_alternative<std::vector<std::int64_t>>(value.value)) {
        const auto& items = std::get<std::vector<std::int64_t>>(value.value);
        sol::table table = lua.create_table(static_cast<int>(items.size()), 0);
        for (std::size_t index = 0; index < items.size(); ++index) {
            table[index + 1] = items[index];
        }
        return sol::make_object(lua, table);
    }

    const auto& items = std::get<std::vector<double>>(value.value);
    sol::table table = lua.create_table(static_cast<int>(items.size()), 0);
    for (std::size_t index = 0; index < items.size(); ++index) {
        table[index + 1] = items[index];
    }
    return sol::make_object(lua, table);
}

sol::table makeStreamFieldsTable(sol::state_view lua, const StreamFieldMap& fields)
{
    sol::table table = lua.create_table(0, static_cast<int>(fields.size()));
    for (const auto& [name, value] : fields) {
        table[name] = streamFieldValueToLua(lua, value);
    }
    return table;
}

sol::table makeStreamFrameTable(sol::state_view lua,
                                const StreamParsedFrame& frame,
                                bool includeRaw,
                                bool includeFieldAliases)
{
    sol::table table = lua.create_table(0, static_cast<int>(frame.fields.size() + 4));
    table["name"] = frame.name;
    if (includeRaw) {
        table["raw"] = makeBytesTable(lua, frame.raw);
    }
    table["crc_ok"] = frame.crcOk;
    if (!frame.channelMap.empty()) {
        sol::table channelMap = lua.create_table(static_cast<int>(frame.channelMap.size()), 0);
        for (std::size_t index = 0; index < frame.channelMap.size(); ++index) {
            channelMap[index + 1] = static_cast<std::int64_t>(frame.channelMap[index] + 1);
        }
        table["channel_map"] = channelMap;
    }
    sol::table fields = lua.create_table(0, static_cast<int>(frame.fields.size()));
    table["fields"] = fields;
    for (const auto& [name, value] : frame.fields) {
        const auto luaValue = streamFieldValueToLua(lua, value);
        fields[name] = luaValue;
        if (includeFieldAliases) {
            table[name] = luaValue;
        }
    }
    return table;
}

sol::table makeStreamFrameArrayTable(sol::state_view lua,
                                     const std::vector<StreamParsedFrame>& frames,
                                     bool includeRaw,
                                     bool includeFieldAliases)
{
    sol::table table = lua.create_table(static_cast<int>(frames.size()), 0);
    for (std::size_t index = 0; index < frames.size(); ++index) {
        table[index + 1] = makeStreamFrameTable(lua, frames[index], includeRaw, includeFieldAliases);
    }
    return table;
}

sol::table makeStreamErrorTable(sol::state_view lua, const StreamParseError& error)
{
    sol::table table = lua.create_table();
    table["code"] = std::string(streamParseErrorCodeName(error.code));
    table["message"] = error.message;
    table["dropped_bytes"] = static_cast<std::int64_t>(error.droppedBytes);
    if (error.frameName.has_value()) {
        table["frame_name"] = *error.frameName;
    }
    if (!error.raw.empty()) {
        table["raw"] = makeBytesTable(lua, error.raw);
    }
    return table;
}

std::shared_ptr<StreamCountExpression> makeConstantCountExpression(std::int64_t value)
{
    auto expression = std::make_shared<StreamCountExpression>();
    expression->op = StreamCountExpressionOp::Constant;
    expression->value = value;
    return expression;
}

std::shared_ptr<StreamCountExpression> makeFieldCountExpression(std::string fieldName)
{
    auto expression = std::make_shared<StreamCountExpression>();
    expression->op = StreamCountExpressionOp::Field;
    expression->fieldName = std::move(fieldName);
    return expression;
}

std::shared_ptr<StreamCountExpression> parseStreamCountExpressionObject(const sol::object& object, std::string& error);

std::shared_ptr<StreamCountExpression> parseStreamCountOperand(const sol::table& table, std::string& error)
{
    const sol::object exprObject = table["expr"];
    if (exprObject.valid() && exprObject.get_type() != sol::type::lua_nil) {
        return parseStreamCountExpressionObject(exprObject, error);
    }

    if (const auto fieldName = luaStringField(table, "field"); fieldName.has_value()) {
        return makeFieldCountExpression(*fieldName);
    }

    const sol::object valueObject = table["left"];
    if (valueObject.valid() && valueObject.get_type() != sol::type::lua_nil) {
        return parseStreamCountExpressionObject(valueObject, error);
    }

    error = "count 表达式缺少 expr 或 field";
    return nullptr;
}

std::shared_ptr<StreamCountExpression> parseConstantCountExpressionTable(const sol::table& table, std::string& error)
{
    const auto value = luaIntegerValue(table["value"]);
    if (!value.has_value()) {
        error = "const count 表达式需要整数 value";
        return nullptr;
    }
    return makeConstantCountExpression(*value);
}

std::shared_ptr<StreamCountExpression> parseFieldCountExpressionTable(const sol::table& table, std::string& error)
{
    const auto fieldName = luaStringField(table, "name").value_or(luaStringField(table, "field").value_or(""));
    if (fieldName.empty()) {
        error = "field count 表达式需要 name";
        return nullptr;
    }
    return makeFieldCountExpression(fieldName);
}

std::optional<StreamCountExpressionOp> parseArithmeticCountOp(const std::string& op)
{
    if (op == "div") {
        return StreamCountExpressionOp::Div;
    }
    if (op == "sub") {
        return StreamCountExpressionOp::Sub;
    }
    if (op == "mul") {
        return StreamCountExpressionOp::Mul;
    }
    return std::nullopt;
}

struct ArithmeticCountArgument {
    std::optional<std::int64_t> value;
    std::shared_ptr<StreamCountExpression> expression;
};

std::optional<ArithmeticCountArgument> parseArithmeticCountArgument(const sol::table& table,
                                                                    const std::string& op,
                                                                    std::string& error)
{
    ArithmeticCountArgument argument;
    if (op == "sub") {
        argument.value = luaIntegerValue(table["value"]);
        if (!argument.value.has_value()) {
            argument.value = luaIntegerValue(table["by"]);
        }
    } else {
        argument.value = luaIntegerValue(table["by"]);
        const sol::object byObject = table["by"];
        if (!argument.value.has_value() && byObject.valid() && byObject.get_type() != sol::type::lua_nil) {
            argument.expression = parseStreamCountExpressionObject(byObject, error);
            if (!argument.expression) {
                return std::nullopt;
            }
        }
    }

    if (!argument.value.has_value() && !argument.expression) {
        error = op + std::string(" count 表达式缺少整数参数");
        return std::nullopt;
    }
    return argument;
}

std::shared_ptr<StreamCountExpression> parseArithmeticCountExpressionTable(const sol::table& table,
                                                                           const std::string& op,
                                                                           std::string& error)
{
    auto operand = parseStreamCountOperand(table, error);
    if (!operand) {
        return nullptr;
    }

    auto argument = parseArithmeticCountArgument(table, op, error);
    if (!argument.has_value()) {
        return nullptr;
    }

    auto expression = std::make_shared<StreamCountExpression>();
    expression->op = *parseArithmeticCountOp(op);
    expression->operand = std::move(operand);
    expression->argument = argument->value.value_or(0);
    expression->argumentExpression = std::move(argument->expression);
    return expression;
}

std::shared_ptr<StreamCountExpression> parseBitCountExpressionTable(const sol::table& table, std::string& error)
{
    auto operand = parseStreamCountOperand(table, error);
    if (!operand) {
        return nullptr;
    }

    auto expression = std::make_shared<StreamCountExpression>();
    expression->op = StreamCountExpressionOp::BitCount;
    expression->operand = std::move(operand);
    return expression;
}

std::shared_ptr<StreamCountExpression> parseRemainingCountExpressionTable(const sol::table& table)
{
    auto expression = std::make_shared<StreamCountExpression>();
    expression->op = StreamCountExpressionOp::Remaining;
    expression->argument = luaIntegerValue(table["unit"]).value_or(0);
    expression->excludeCrc = luaBoolField(table, "exclude_crc").value_or(true);
    return expression;
}

std::shared_ptr<StreamCountExpression> parseIfFlagCountExpressionTable(const sol::table& table, std::string& error)
{
    const auto fieldName = luaStringField(table, "field");
    const auto mask = luaIntegerValue(table["mask"]);
    if (!fieldName.has_value() || !mask.has_value()) {
        error = "if_flag count 表达式需要 field 和 mask";
        return nullptr;
    }
    const sol::object thenObject = table["then"];
    const sol::object elseObject = table["else"];
    auto thenExpression = parseStreamCountExpressionObject(thenObject, error);
    if (!thenExpression) {
        return nullptr;
    }
    auto elseExpression = parseStreamCountExpressionObject(elseObject, error);
    if (!elseExpression) {
        return nullptr;
    }

    auto expression = std::make_shared<StreamCountExpression>();
    expression->op = StreamCountExpressionOp::IfFlag;
    expression->fieldName = *fieldName;
    expression->argument = *mask;
    expression->thenExpression = std::move(thenExpression);
    expression->elseExpression = std::move(elseExpression);
    return expression;
}

bool parseCaseCountEntries(const sol::table& casesTable, StreamCountExpression& expression, std::string& error)
{
    for (const auto& item : casesTable) {
        const auto key = luaIntegerValue(item.first);
        if (!key.has_value()) {
            error = "case.cases 的 key 必须是整数";
            return false;
        }
        auto caseExpression = parseStreamCountExpressionObject(item.second, error);
        if (!caseExpression) {
            return false;
        }
        expression.cases.push_back(StreamCountCase{.value = *key, .expression = std::move(caseExpression)});
    }
    return true;
}

std::shared_ptr<StreamCountExpression> parseCaseCountExpressionTable(const sol::table& table, std::string& error)
{
    const auto fieldName = luaStringField(table, "field");
    if (!fieldName.has_value()) {
        error = "case count 表达式需要 field";
        return nullptr;
    }
    const sol::object casesObject = table["cases"];
    if (!casesObject.valid() || !casesObject.is<sol::table>()) {
        error = "case count 表达式需要 cases table";
        return nullptr;
    }

    auto expression = std::make_shared<StreamCountExpression>();
    expression->op = StreamCountExpressionOp::Case;
    expression->fieldName = *fieldName;

    if (!parseCaseCountEntries(casesObject.as<sol::table>(), *expression, error)) {
        return nullptr;
    }

    const sol::object defaultObject = table["default"];
    if (defaultObject.valid() && defaultObject.get_type() != sol::type::lua_nil) {
        expression->defaultExpression = parseStreamCountExpressionObject(defaultObject, error);
        if (!expression->defaultExpression) {
            return nullptr;
        }
    }
    return expression;
}

std::shared_ptr<StreamCountExpression> parseStreamCountExpressionTable(const sol::table& table, std::string& error)
{
    const auto op = luaStringField(table, "op");
    if (!op.has_value()) {
        error = "count 表达式缺少 op";
        return nullptr;
    }

    if (*op == "const" || *op == "value") {
        return parseConstantCountExpressionTable(table, error);
    }
    if (*op == "field") {
        return parseFieldCountExpressionTable(table, error);
    }
    if (parseArithmeticCountOp(*op).has_value()) {
        return parseArithmeticCountExpressionTable(table, *op, error);
    }
    if (*op == "bit_count") {
        return parseBitCountExpressionTable(table, error);
    }
    if (*op == "remaining") {
        return parseRemainingCountExpressionTable(table);
    }
    if (*op == "if_flag") {
        return parseIfFlagCountExpressionTable(table, error);
    }
    if (*op == "case") {
        return parseCaseCountExpressionTable(table, error);
    }

    error = "未知 count 表达式 op: " + *op;
    return nullptr;
}

std::shared_ptr<StreamCountExpression> parseStreamCountExpressionObject(const sol::object& object, std::string& error)
{
    if (!object.valid() || object.get_type() == sol::type::lua_nil) {
        error = "count 表达式不能为空";
        return nullptr;
    }
    if (const auto value = luaIntegerValue(object); value.has_value()) {
        return makeConstantCountExpression(*value);
    }
    if (object.is<std::string>()) {
        return makeFieldCountExpression(object.as<std::string>());
    }
    if (object.is<sol::table>()) {
        return parseStreamCountExpressionTable(object.as<sol::table>(), error);
    }

    error = "count 表达式仅支持整数、字段名或 table";
    return nullptr;
}

bool parseStreamBufferDefinition(const sol::table& schemaTable,
                                 StreamBufferDefinition& bufferDefinition,
                                 std::string& error)
{
    if (const sol::object bufferObject = schemaTable["buffer"];
        bufferObject.valid() && bufferObject.get_type() != sol::type::lua_nil) {
        if (!bufferObject.is<sol::table>()) {
            error = "stream.buffer 必须是 table";
            return false;
        }
        const auto bufferTable = bufferObject.as<sol::table>();
        if (const auto capacity = luaIntegerValue(bufferTable["capacity"]); capacity.has_value()) {
            if (*capacity <= 0) {
                error = "stream.buffer.capacity 必须大于 0";
                return false;
            }
            bufferDefinition.capacity = static_cast<std::size_t>(*capacity);
            bufferDefinition.maxCapacity = (std::max)(bufferDefinition.maxCapacity, bufferDefinition.capacity);
        }
        if (const auto maxCapacity = luaIntegerValue(bufferTable["max_capacity"]); maxCapacity.has_value()) {
            if (*maxCapacity <= 0) {
                error = "stream.buffer.max_capacity 必须大于 0";
                return false;
            }
            bufferDefinition.maxCapacity =
                (std::max)(static_cast<std::size_t>(*maxCapacity), bufferDefinition.capacity);
        }
        if (const auto overflow = luaStringField(bufferTable, "overflow"); overflow.has_value()) {
            if (*overflow != "drop_oldest") {
                error = "stream.buffer.overflow 目前仅支持 drop_oldest";
                return false;
            }
            bufferDefinition.dropOldest = true;
        }
    }
    return true;
}

bool applyRawFrameOutputOption(const sol::table& schemaTable, LoadedStreamSchema& loaded, std::string& error)
{
    const auto rawOutput = luaStringField(schemaTable, "raw_output");
    if (!rawOutput.has_value()) {
        return true;
    }
    if (*rawOutput == "omit") {
        loaded.includeRawFrames = false;
        return true;
    }
    if (*rawOutput == "full") {
        return true;
    }
    error = "stream.raw_output 必须是 'full' 或 'omit'";
    return false;
}

bool applyFieldOutputOption(const sol::table& schemaTable, LoadedStreamSchema& loaded, std::string& error)
{
    const auto fieldOutput = luaStringField(schemaTable, "field_output");
    if (!fieldOutput.has_value()) {
        return true;
    }
    if (*fieldOutput == "fields_only") {
        loaded.includeFieldAliases = false;
        return true;
    }
    if (*fieldOutput == "compat") {
        return true;
    }
    error = "stream.field_output 必须是 'compat' 或 'fields_only'";
    return false;
}

bool registerStreamBatchCallback(const sol::table& schemaTable,
                                 LoadedStreamSchema& loaded,
                                 std::unordered_map<std::string, sol::protected_function>& callbacks,
                                 std::string& error)
{
    const sol::object onBatchObject = schemaTable["on_batch"];
    if (!onBatchObject.valid() || onBatchObject.get_type() == sol::type::lua_nil) {
        return true;
    }
    if (!onBatchObject.is<sol::protected_function>()) {
        error = "stream.on_batch 必须是 function";
        return false;
    }
    const std::string onBatchCallbackKey = "stream.on_batch";
    callbacks.insert_or_assign(onBatchCallbackKey, onBatchObject.as<sol::protected_function>());
    loaded.onBatchCallbackKey = onBatchCallbackKey;
    return true;
}

bool applyLoadedStreamOptions(const sol::table& schemaTable,
                              LoadedStreamSchema& loaded,
                              std::unordered_map<std::string, sol::protected_function>& callbacks,
                              std::string& error)
{
    if (!applyRawFrameOutputOption(schemaTable, loaded, error)) {
        return false;
    }
    if (const auto lowOverhead = luaBoolField(schemaTable, "low_overhead"); lowOverhead.has_value()) {
        loaded.lowOverhead = *lowOverhead;
    }
    if (!applyFieldOutputOption(schemaTable, loaded, error)) {
        return false;
    }
    return registerStreamBatchCallback(schemaTable, loaded, callbacks, error);
}

bool parseStreamFrameHeader(const sol::table& frameTable, StreamFrameDefinition& frame, std::string& error)
{
    std::string bytesError;
    const auto header = bytesFromLuaObject(frameTable["header"], bytesError);
    if (!header.has_value() || header->empty()) {
        error = "frame.header 必须是非空 byte[]";
        return false;
    }
    frame.header = *header;
    return true;
}

bool parseStreamFrameLengthDefinition(const sol::object& lenObject,
                                      StreamLengthDefinition& lenDefinition,
                                      std::string& error)
{
    if (!lenObject.is<sol::table>()) {
        error = "frame.len 必须是 table";
        return false;
    }
    const auto lenTable = lenObject.as<sol::table>();
    const auto offset = luaIntegerValue(lenTable["offset"]);
    if (!offset.has_value() || *offset <= 0) {
        error = "frame.len.offset 必须是从 1 开始的正整数";
        return false;
    }
    lenDefinition.offset = static_cast<std::size_t>(*offset - 1);

    const auto typeText = luaStringField(lenTable, "type");
    if (!typeText.has_value()) {
        error = "frame.len.type 不能为空";
        return false;
    }
    const auto valueType = parseStreamValueType(*typeText);
    if (!valueType.has_value() || !streamValueTypeCanBeLength(*valueType)) {
        error = "frame.len.type 必须是整数类型";
        return false;
    }
    lenDefinition.type = *valueType;

    const auto meansText = luaStringField(lenTable, "means").value_or("payload");
    const auto means = parseStreamLengthMeans(meansText);
    if (!means.has_value()) {
        error = "frame.len.means 仅支持 payload 或 frame";
        return false;
    }
    lenDefinition.means = *means;
    if (const auto extra = luaIntegerValue(lenTable["extra"]); extra.has_value()) {
        if (*extra < 0) {
            error = "frame.len.extra 不能为负数";
            return false;
        }
        lenDefinition.extra = static_cast<std::size_t>(*extra);
    }
    return true;
}

bool parseStreamFrameSizeMode(const sol::table& frameTable, StreamFrameDefinition& frame, std::string& error)
{
    const auto fixedSize = luaIntegerValue(frameTable["size"]);
    const sol::object lenObject = frameTable["len"];
    const bool hasLen = lenObject.valid() && lenObject.get_type() != sol::type::lua_nil;
    const bool runtimeProfile = luaBoolField(frameTable, "runtime_profile").value_or(false);
    const int modeCount =
        static_cast<int>(fixedSize.has_value()) + static_cast<int>(hasLen) + static_cast<int>(runtimeProfile);
    if (modeCount != 1) {
        error = "frame.size、frame.len 与 frame.runtime_profile 必须三选一";
        return false;
    }
    frame.runtimeProfile = runtimeProfile;
    if (fixedSize.has_value()) {
        if (*fixedSize <= 0) {
            error = "frame.size 必须大于 0";
            return false;
        }
        frame.size = static_cast<std::size_t>(*fixedSize);
    } else if (hasLen) {
        StreamLengthDefinition lenDefinition;
        if (!parseStreamFrameLengthDefinition(lenObject, lenDefinition, error)) {
            return false;
        }
        frame.len = lenDefinition;
    }
    return true;
}

bool parseStreamFrameCrcTable(const sol::table& crcTable, StreamFrameDefinition& frame, std::string& error)
{
    const auto crcTypeText = luaStringField(crcTable, "type");
    if (!crcTypeText.has_value()) {
        error = "frame.crc.type 不能为空";
        return false;
    }
    const auto crcType = parseStreamCrcType(*crcTypeText);
    if (!crcType.has_value()) {
        error = "未知 frame.crc.type: " + *crcTypeText;
        return false;
    }

    const auto orderText = luaStringField(crcTable, "order").value_or("lo_hi");
    const auto order = parseStreamCrcOrder(orderText);
    if (!order.has_value()) {
        error = "frame.crc.order 仅支持 lo_hi 或 hi_lo";
        return false;
    }

    frame.crc.type = *crcType;
    frame.crc.order = *order;
    return true;
}

bool parseStreamFrameCrcDefinition(const sol::table& frameTable, StreamFrameDefinition& frame, std::string& error)
{
    if (const sol::object crcObject = frameTable["crc"];
        crcObject.valid() && crcObject.get_type() != sol::type::lua_nil) {
        if (crcObject.is<bool>() && !crcObject.as<bool>()) {
            frame.crc.type = StreamCrcType::None;
        } else if (crcObject.is<sol::table>()) {
            if (!parseStreamFrameCrcTable(crcObject.as<sol::table>(), frame, error)) {
                return false;
            }
        } else {
            error = "frame.crc 必须是 table 或 false";
            return false;
        }
    }
    return true;
}

bool parseStreamFieldCount(const sol::object& countObject,
                           const std::size_t frameIndex,
                           const std::size_t fieldIndex,
                           StreamFieldDefinition& field,
                           std::string& error)
{
    if (!countObject.valid() || countObject.get_type() == sol::type::lua_nil) {
        return true;
    }

    const auto countPath = streamFieldCountPath(frameIndex, fieldIndex);
    if (const auto count = luaIntegerValue(countObject); count.has_value()) {
        if (*count < 0) {
            error = countPath + " 不能为负数";
            return false;
        }
        field.count.fixed = static_cast<std::size_t>(*count);
    } else if (countObject.is<std::string>()) {
        field.count.fieldName = countObject.as<std::string>();
    } else if (countObject.is<sol::protected_function>()) {
        error = countPath + " 检测到 function；该写法已废弃，不再支持 function。"
                            " 请迁移为 count 表达式 table，例如 count = { op = \"div\", field = "
                            "\"byte_count\", by = 2 }";
        return false;
    } else if (countObject.is<sol::table>()) {
        field.count.expression = parseStreamCountExpressionObject(countObject, error);
        if (!field.count.expression) {
            error = countPath + " 表达式无效: " + error;
            return false;
        }
    } else {
        error = countPath + " 仅支持整数、字段名或 count 表达式 table";
        return false;
    }
    return true;
}

std::optional<StreamFieldDefinition> parseStreamFieldDefinition(const sol::object& fieldObject,
                                                                const std::size_t frameIndex,
                                                                const std::size_t fieldIndex,
                                                                std::string& error)
{
    if (!fieldObject.valid() || !fieldObject.is<sol::table>()) {
        error = "frame.fields 元素必须是 table";
        return std::nullopt;
    }
    const auto fieldTable = fieldObject.as<sol::table>();
    StreamFieldDefinition field;
    field.name = fieldTable.get_or("name", std::string());
    if (field.name.empty()) {
        error = "field.name 不能为空";
        return std::nullopt;
    }

    const auto typeText = luaStringField(fieldTable, "type");
    if (!typeText.has_value()) {
        error = "field.type 不能为空";
        return std::nullopt;
    }
    const auto type = parseStreamValueType(*typeText);
    if (!type.has_value()) {
        error = "未知字段类型: " + *typeText;
        return std::nullopt;
    }
    field.type = *type;

    if (const auto offset = luaIntegerValue(fieldTable["offset"]); offset.has_value()) {
        if (*offset <= 0) {
            error = "field.offset 必须是从 1 开始的正整数";
            return std::nullopt;
        }
        field.offset = static_cast<std::size_t>(*offset - 1);
    }

    if (!parseStreamFieldCount(fieldTable["count"], frameIndex, fieldIndex, field, error)) {
        return std::nullopt;
    }
    return field;
}

bool parseStreamFrameFields(const sol::table& frameTable,
                            const std::size_t frameIndex,
                            StreamFrameDefinition& frame,
                            std::string& error)
{
    if (const sol::object fieldsObject = frameTable["fields"];
        fieldsObject.valid() && fieldsObject.get_type() != sol::type::lua_nil) {
        if (!fieldsObject.is<sol::table>()) {
            error = "frame.fields 必须是数组";
            return false;
        }
        const auto fieldsTable = fieldsObject.as<sol::table>();
        frame.fields.reserve(fieldsTable.size());
        for (std::size_t fieldIndex = 1; fieldIndex <= fieldsTable.size(); ++fieldIndex) {
            auto field = parseStreamFieldDefinition(fieldsTable[fieldIndex], frameIndex, fieldIndex - 1, error);
            if (!field.has_value()) {
                return false;
            }
            frame.fields.push_back(std::move(*field));
        }
    }
    return true;
}

bool registerStreamFrameCallback(const sol::table& frameTable,
                                 const StreamFrameDefinition& frame,
                                 LoadedStreamSchema& loaded,
                                 std::unordered_map<std::string, sol::protected_function>& callbacks,
                                 std::string& error)
{
    if (const sol::object onFrameObject = frameTable["on_frame"];
        onFrameObject.valid() && onFrameObject.get_type() != sol::type::lua_nil) {
        if (!onFrameObject.is<sol::protected_function>()) {
            error = "frame.on_frame 必须是 function";
            return false;
        }
        const auto callbackKey = frameOnFrameCallbackKey(frame.name);
        callbacks.insert_or_assign(callbackKey, onFrameObject.as<sol::protected_function>());
        loaded.frameCallbackKeys.insert_or_assign(frame.name, callbackKey);
    } else if (!loaded.onBatchCallbackKey.has_value()) {
        error = "frame.on_frame 必须是 function，除非 stream.on_batch 已定义";
        return false;
    }
    return true;
}

std::optional<StreamFrameDefinition> parseStreamFrameDefinition(
    const sol::object& frameObject,
    const std::size_t frameIndex,
    LoadedStreamSchema& loaded,
    std::unordered_map<std::string, sol::protected_function>& callbacks,
    std::unordered_set<std::string>& frameNames,
    std::string& error)
{
    if (!frameObject.valid() || !frameObject.is<sol::table>()) {
        error = "stream.frames 元素必须是 table";
        return std::nullopt;
    }
    const auto frameTable = frameObject.as<sol::table>();

    StreamFrameDefinition frame;
    frame.name = frameTable.get_or("name", std::string());
    if (frame.name.empty()) {
        error = "stream.frames[].name 不能为空";
        return std::nullopt;
    }
    if (!frameNames.insert(frame.name).second) {
        error = "stream.frames[].name 不能重复: " + frame.name;
        return std::nullopt;
    }

    if (!parseStreamFrameHeader(frameTable, frame, error) || !parseStreamFrameSizeMode(frameTable, frame, error) ||
        !parseStreamFrameCrcDefinition(frameTable, frame, error) ||
        !parseStreamFrameFields(frameTable, frameIndex, frame, error) ||
        !registerStreamFrameCallback(frameTable, frame, loaded, callbacks, error)) {
        return std::nullopt;
    }
    return frame;
}

bool registerStreamErrorCallback(const sol::table& schemaTable,
                                 LoadedStreamSchema& loaded,
                                 std::unordered_map<std::string, sol::protected_function>& callbacks,
                                 std::string& error)
{
    if (const sol::object onErrorObject = schemaTable["on_error"];
        onErrorObject.valid() && onErrorObject.get_type() != sol::type::lua_nil) {
        if (!onErrorObject.is<sol::protected_function>()) {
            error = "stream.on_error 必须是 function";
            return false;
        }
        const std::string onErrorCallbackKey = "stream.on_error";
        callbacks.insert_or_assign(onErrorCallbackKey, onErrorObject.as<sol::protected_function>());
        loaded.onErrorCallbackKey = onErrorCallbackKey;
    }
    return true;
}

std::optional<sol::table> loadStreamSchemaTable(sol::state_view lua, std::string& error)
{
    const sol::object streamObject = lua["stream"];
    if (!streamObject.valid() || streamObject.get_type() == sol::type::lua_nil) {
        return std::nullopt;
    }
    if (!streamObject.is<sol::protected_function>()) {
        error = "stream 必须是 function";
        return std::nullopt;
    }

    auto streamFunction = streamObject.as<sol::protected_function>();
    auto streamResult = streamFunction();
    if (!streamResult.valid()) {
        error = "stream() 执行失败: " + protectedCallError(streamResult);
        return std::nullopt;
    }

    const sol::object schemaObject = streamResult.get<sol::object>();
    if (!schemaObject.valid() || schemaObject.get_type() == sol::type::lua_nil) {
        return std::nullopt;
    }
    if (!schemaObject.is<sol::table>()) {
        error = "stream() 必须返回 table";
        return std::nullopt;
    }
    return schemaObject.as<sol::table>();
}

std::optional<sol::table> parseStreamFramesTable(const sol::table& schemaTable, std::string& error)
{
    const sol::object framesObject = schemaTable["frames"];
    if (!framesObject.valid() || !framesObject.is<sol::table>()) {
        error = "stream.frames 必须是非空数组";
        return std::nullopt;
    }

    const auto framesTable = framesObject.as<sol::table>();
    if (framesTable.size() == 0) {
        error = "stream.frames 不能为空";
        return std::nullopt;
    }
    return framesTable;
}

bool parseStreamFrameDefinitions(const sol::table& framesTable,
                                 LoadedStreamSchema& loaded,
                                 std::unordered_map<std::string, sol::protected_function>& callbacks,
                                 std::vector<StreamFrameDefinition>& frames,
                                 std::string& error)
{
    std::unordered_set<std::string> frameNames;
    frames.reserve(framesTable.size());
    for (std::size_t index = 1; index <= framesTable.size(); ++index) {
        auto frame = parseStreamFrameDefinition(framesTable[index], index - 1, loaded, callbacks, frameNames, error);
        if (!frame.has_value()) {
            return false;
        }
        frames.push_back(std::move(*frame));
    }
    return true;
}

std::unique_ptr<LoadedStreamSchema> parseLoadedStreamSchema(
    sol::state_view lua, std::unordered_map<std::string, sol::protected_function>& callbacks, std::string& error)
{
    const auto schemaTable = loadStreamSchemaTable(lua, error);
    if (!schemaTable.has_value()) {
        return nullptr;
    }

    StreamBufferDefinition bufferDefinition;
    if (!parseStreamBufferDefinition(*schemaTable, bufferDefinition, error)) {
        return nullptr;
    }

    const auto framesTable = parseStreamFramesTable(*schemaTable, error);
    if (!framesTable.has_value()) {
        return nullptr;
    }

    std::vector<StreamFrameDefinition> frames;
    auto loaded = std::make_unique<LoadedStreamSchema>(bufferDefinition, std::vector<StreamFrameDefinition>{});
    if (!applyLoadedStreamOptions(*schemaTable, *loaded, callbacks, error)) {
        return nullptr;
    }

    if (!parseStreamFrameDefinitions(*framesTable, *loaded, callbacks, frames, error)) {
        return nullptr;
    }

    if (!registerStreamErrorCallback(*schemaTable, *loaded, callbacks, error)) {
        return nullptr;
    }

    loaded->parser = FrameStreamParser(bufferDefinition, std::move(frames));
    return loaded;
}

} // namespace protoscope::scripting
