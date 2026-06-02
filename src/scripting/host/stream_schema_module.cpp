#include "script_host_internal.hpp"

#include <unordered_set>
#include <utility>

namespace protoscope::scripting {

std::string streamFieldCountPath(const std::size_t frameIndex, const std::size_t fieldIndex) {
    return "stream.frames[" + std::to_string(frameIndex + 1) + "].fields[" + std::to_string(fieldIndex + 1) + "].count";
}

std::string frameOnFrameCallbackKey(const std::string& frameName) {
    return "stream.frame." + frameName + ".on_frame";
}

std::optional<StreamValueType> parseStreamValueType(const std::string& text) {
    if (text == "u8") return StreamValueType::U8;
    if (text == "i8") return StreamValueType::I8;
    if (text == "u16_be") return StreamValueType::U16Be;
    if (text == "u16_le") return StreamValueType::U16Le;
    if (text == "i16_be") return StreamValueType::I16Be;
    if (text == "i16_le") return StreamValueType::I16Le;
    if (text == "u32_be") return StreamValueType::U32Be;
    if (text == "u32_le") return StreamValueType::U32Le;
    if (text == "i32_be") return StreamValueType::I32Be;
    if (text == "i32_le") return StreamValueType::I32Le;
    if (text == "f32_be") return StreamValueType::F32Be;
    if (text == "f32_le") return StreamValueType::F32Le;
    if (text == "bytes") return StreamValueType::Bytes;
    return std::nullopt;
}

bool streamValueTypeCanBeLength(StreamValueType type) {
    return type != StreamValueType::Bytes && !streamValueTypeIsFloat(type);
}

std::optional<StreamLengthMeans> parseStreamLengthMeans(const std::string& text) {
    if (text == "payload") {
        return StreamLengthMeans::Payload;
    }
    if (text == "frame") {
        return StreamLengthMeans::Frame;
    }
    return std::nullopt;
}

std::optional<StreamCrcType> parseStreamCrcType(const std::string& text) {
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

std::optional<StreamCrcOrder> parseStreamCrcOrder(const std::string& text) {
    if (text == "hi_lo") {
        return StreamCrcOrder::HiLo;
    }
    if (text == "lo_hi") {
        return StreamCrcOrder::LoHi;
    }
    return std::nullopt;
}

sol::object streamFieldValueToLua(sol::state_view lua, const StreamFieldValue& value) {
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

sol::table makeStreamFieldsTable(sol::state_view lua, const StreamFieldMap& fields) {
    sol::table table = lua.create_table();
    for (const auto& [name, value] : fields) {
        table[name] = streamFieldValueToLua(lua, value);
    }
    return table;
}

sol::table makeStreamFrameTable(sol::state_view lua, const StreamParsedFrame& frame) {
    sol::table table = lua.create_table();
    table["name"] = frame.name;
    table["raw"] = makeBytesTable(lua, frame.raw);
    table["crc_ok"] = frame.crcOk;
    if (!frame.channelMap.empty()) {
        sol::table channelMap = lua.create_table(static_cast<int>(frame.channelMap.size()), 0);
        for (std::size_t index = 0; index < frame.channelMap.size(); ++index) {
            channelMap[index + 1] = static_cast<std::int64_t>(frame.channelMap[index] + 1);
        }
        table["channel_map"] = channelMap;
    }
    const auto fields = makeStreamFieldsTable(lua, frame.fields);
    table["fields"] = fields;
    for (const auto& [name, value] : frame.fields) {
        table[name] = streamFieldValueToLua(lua, value);
    }
    return table;
}

sol::table makeStreamErrorTable(sol::state_view lua, const StreamParseError& error) {
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

std::shared_ptr<StreamCountExpression> makeConstantCountExpression(std::int64_t value) {
    auto expression = std::make_shared<StreamCountExpression>();
    expression->op = StreamCountExpressionOp::Constant;
    expression->value = value;
    return expression;
}

std::shared_ptr<StreamCountExpression> makeFieldCountExpression(std::string fieldName) {
    auto expression = std::make_shared<StreamCountExpression>();
    expression->op = StreamCountExpressionOp::Field;
    expression->fieldName = std::move(fieldName);
    return expression;
}

std::shared_ptr<StreamCountExpression> parseStreamCountExpressionObject(const sol::object& object, std::string& error);

std::shared_ptr<StreamCountExpression> parseStreamCountOperand(const sol::table& table, std::string& error) {
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

std::shared_ptr<StreamCountExpression> parseStreamCountExpressionTable(const sol::table& table, std::string& error) {
    const auto op = luaStringField(table, "op");
    if (!op.has_value()) {
        error = "count 表达式缺少 op";
        return nullptr;
    }

    if (*op == "const" || *op == "value") {
        const auto value = luaIntegerValue(table["value"]);
        if (!value.has_value()) {
            error = "const count 表达式需要整数 value";
            return nullptr;
        }
        return makeConstantCountExpression(*value);
    }

    if (*op == "field") {
        const auto fieldName = luaStringField(table, "name").value_or(luaStringField(table, "field").value_or(""));
        if (fieldName.empty()) {
            error = "field count 表达式需要 name";
            return nullptr;
        }
        return makeFieldCountExpression(fieldName);
    }

    if (*op == "div" || *op == "sub" || *op == "mul") {
        auto operand = parseStreamCountOperand(table, error);
        if (!operand) {
            return nullptr;
        }

        std::optional<std::int64_t> argument;
        std::shared_ptr<StreamCountExpression> argumentExpression;
        if (*op == "sub") {
            argument = luaIntegerValue(table["value"]);
            if (!argument.has_value()) {
                argument = luaIntegerValue(table["by"]);
            }
        } else {
            argument = luaIntegerValue(table["by"]);
            const sol::object byObject = table["by"];
            if (!argument.has_value() && byObject.valid() && byObject.get_type() != sol::type::lua_nil) {
                argumentExpression = parseStreamCountExpressionObject(byObject, error);
                if (!argumentExpression) {
                    return nullptr;
                }
            }
        }
        if (!argument.has_value() && !argumentExpression) {
            error = *op + std::string(" count 表达式缺少整数参数");
            return nullptr;
        }

        auto expression = std::make_shared<StreamCountExpression>();
        expression->op = *op == "div" ? StreamCountExpressionOp::Div
            : (*op == "sub" ? StreamCountExpressionOp::Sub : StreamCountExpressionOp::Mul);
        expression->operand = std::move(operand);
        expression->argument = argument.value_or(0);
        expression->argumentExpression = std::move(argumentExpression);
        return expression;
    }

    if (*op == "bit_count") {
        auto operand = parseStreamCountOperand(table, error);
        if (!operand) {
            return nullptr;
        }
        auto expression = std::make_shared<StreamCountExpression>();
        expression->op = StreamCountExpressionOp::BitCount;
        expression->operand = std::move(operand);
        return expression;
    }

    if (*op == "remaining") {
        auto expression = std::make_shared<StreamCountExpression>();
        expression->op = StreamCountExpressionOp::Remaining;
        expression->argument = luaIntegerValue(table["unit"]).value_or(0);
        expression->excludeCrc = luaBoolField(table, "exclude_crc").value_or(true);
        return expression;
    }

    if (*op == "if_flag") {
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

    if (*op == "case") {
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

        const auto casesTable = casesObject.as<sol::table>();
        for (const auto& item : casesTable) {
            const auto key = luaIntegerValue(item.first);
            if (!key.has_value()) {
                error = "case.cases 的 key 必须是整数";
                return nullptr;
            }
            auto caseExpression = parseStreamCountExpressionObject(item.second, error);
            if (!caseExpression) {
                return nullptr;
            }
            expression->cases.push_back(StreamCountCase{.value = *key, .expression = std::move(caseExpression)});
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

    error = "未知 count 表达式 op: " + *op;
    return nullptr;
}

std::shared_ptr<StreamCountExpression> parseStreamCountExpressionObject(const sol::object& object, std::string& error) {
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

std::unique_ptr<LoadedStreamSchema> parseLoadedStreamSchema(
    sol::state_view lua,
    std::unordered_map<std::string, sol::protected_function>& callbacks,
    std::string& error) {
    const sol::object streamObject = lua["stream"];
    if (!streamObject.valid() || streamObject.get_type() == sol::type::lua_nil) {
        return nullptr;
    }
    if (!streamObject.is<sol::protected_function>()) {
        error = "stream 必须是 function";
        return nullptr;
    }

    auto streamFunction = streamObject.as<sol::protected_function>();
    auto streamResult = streamFunction();
    if (!streamResult.valid()) {
        error = "stream() 执行失败: " + protectedCallError(streamResult);
        return nullptr;
    }

    const sol::object schemaObject = streamResult.get<sol::object>();
    if (!schemaObject.valid() || schemaObject.get_type() == sol::type::lua_nil) {
        return nullptr;
    }
    if (!schemaObject.is<sol::table>()) {
        error = "stream() 必须返回 table";
        return nullptr;
    }

    const auto schemaTable = schemaObject.as<sol::table>();
    StreamBufferDefinition bufferDefinition;
    if (const sol::object bufferObject = schemaTable["buffer"]; bufferObject.valid() && bufferObject.get_type() != sol::type::lua_nil) {
        if (!bufferObject.is<sol::table>()) {
            error = "stream.buffer 必须是 table";
            return nullptr;
        }
        const auto bufferTable = bufferObject.as<sol::table>();
        if (const auto capacity = luaIntegerValue(bufferTable["capacity"]); capacity.has_value()) {
            if (*capacity <= 0) {
                error = "stream.buffer.capacity 必须大于 0";
                return nullptr;
            }
            bufferDefinition.capacity = static_cast<std::size_t>(*capacity);
        }
        if (const auto overflow = luaStringField(bufferTable, "overflow"); overflow.has_value()) {
            if (*overflow != "drop_oldest") {
                error = "stream.buffer.overflow 目前仅支持 drop_oldest";
                return nullptr;
            }
            bufferDefinition.dropOldest = true;
        }
    }

    const sol::object framesObject = schemaTable["frames"];
    if (!framesObject.valid() || !framesObject.is<sol::table>()) {
        error = "stream.frames 必须是非空数组";
        return nullptr;
    }

    const auto framesTable = framesObject.as<sol::table>();
    if (framesTable.size() == 0) {
        error = "stream.frames 不能为空";
        return nullptr;
    }

    std::vector<StreamFrameDefinition> frames;
    frames.reserve(framesTable.size());
    auto loaded = std::make_unique<LoadedStreamSchema>(bufferDefinition, std::vector<StreamFrameDefinition>{});
    std::unordered_set<std::string> frameNames;

    for (std::size_t index = 1; index <= framesTable.size(); ++index) {
        const sol::object frameObject = framesTable[index];
        if (!frameObject.valid() || !frameObject.is<sol::table>()) {
            error = "stream.frames 元素必须是 table";
            return nullptr;
        }
        const auto frameTable = frameObject.as<sol::table>();

        StreamFrameDefinition frame;
        frame.name = frameTable.get_or("name", std::string());
        if (frame.name.empty()) {
            error = "stream.frames[].name 不能为空";
            return nullptr;
        }
        if (!frameNames.insert(frame.name).second) {
            error = "stream.frames[].name 不能重复: " + frame.name;
            return nullptr;
        }

        std::string bytesError;
        const auto header = bytesFromLuaObject(frameTable["header"], bytesError);
        if (!header.has_value() || header->empty()) {
            error = "frame.header 必须是非空 byte[]";
            return nullptr;
        }
        frame.header = *header;

        const auto fixedSize = luaIntegerValue(frameTable["size"]);
        const sol::object lenObject = frameTable["len"];
        const bool hasLen = lenObject.valid() && lenObject.get_type() != sol::type::lua_nil;
        const bool runtimeProfile = luaBoolField(frameTable, "runtime_profile").value_or(false);
        const int modeCount = static_cast<int>(fixedSize.has_value()) + static_cast<int>(hasLen) + static_cast<int>(runtimeProfile);
        if (modeCount != 1) {
            error = "frame.size、frame.len 与 frame.runtime_profile 必须三选一";
            return nullptr;
        }
        frame.runtimeProfile = runtimeProfile;
        if (fixedSize.has_value()) {
            if (*fixedSize <= 0) {
                error = "frame.size 必须大于 0";
                return nullptr;
            }
            frame.size = static_cast<std::size_t>(*fixedSize);
        } else if (hasLen) {
            if (!lenObject.is<sol::table>()) {
                error = "frame.len 必须是 table";
                return nullptr;
            }
            const auto lenTable = lenObject.as<sol::table>();
            StreamLengthDefinition lenDefinition;
            const auto offset = luaIntegerValue(lenTable["offset"]);
            if (!offset.has_value() || *offset <= 0) {
                error = "frame.len.offset 必须是从 1 开始的正整数";
                return nullptr;
            }
            lenDefinition.offset = static_cast<std::size_t>(*offset - 1);

            const auto typeText = luaStringField(lenTable, "type");
            if (!typeText.has_value()) {
                error = "frame.len.type 不能为空";
                return nullptr;
            }
            const auto valueType = parseStreamValueType(*typeText);
            if (!valueType.has_value() || !streamValueTypeCanBeLength(*valueType)) {
                error = "frame.len.type 必须是整数类型";
                return nullptr;
            }
            lenDefinition.type = *valueType;

            const auto meansText = luaStringField(lenTable, "means").value_or("payload");
            const auto means = parseStreamLengthMeans(meansText);
            if (!means.has_value()) {
                error = "frame.len.means 仅支持 payload 或 frame";
                return nullptr;
            }
            lenDefinition.means = *means;
            if (const auto extra = luaIntegerValue(lenTable["extra"]); extra.has_value()) {
                if (*extra < 0) {
                    error = "frame.len.extra 不能为负数";
                    return nullptr;
                }
                lenDefinition.extra = static_cast<std::size_t>(*extra);
            }
            frame.len = lenDefinition;
        }

        if (const sol::object crcObject = frameTable["crc"]; crcObject.valid() && crcObject.get_type() != sol::type::lua_nil) {
            if (crcObject.is<bool>() && !crcObject.as<bool>()) {
                frame.crc.type = StreamCrcType::None;
            } else if (crcObject.is<sol::table>()) {
                const auto crcTable = crcObject.as<sol::table>();
                const auto crcTypeText = luaStringField(crcTable, "type");
                if (!crcTypeText.has_value()) {
                    error = "frame.crc.type 不能为空";
                    return nullptr;
                }
                const auto crcType = parseStreamCrcType(*crcTypeText);
                if (!crcType.has_value()) {
                    error = "未知 frame.crc.type: " + *crcTypeText;
                    return nullptr;
                }
                frame.crc.type = *crcType;
                const auto orderText = luaStringField(crcTable, "order").value_or("lo_hi");
                const auto order = parseStreamCrcOrder(orderText);
                if (!order.has_value()) {
                    error = "frame.crc.order 仅支持 lo_hi 或 hi_lo";
                    return nullptr;
                }
                frame.crc.order = *order;
            } else {
                error = "frame.crc 必须是 table 或 false";
                return nullptr;
            }
        }

        if (const sol::object fieldsObject = frameTable["fields"]; fieldsObject.valid() && fieldsObject.get_type() != sol::type::lua_nil) {
            if (!fieldsObject.is<sol::table>()) {
                error = "frame.fields 必须是数组";
                return nullptr;
            }
            const auto fieldsTable = fieldsObject.as<sol::table>();
            frame.fields.reserve(fieldsTable.size());
            for (std::size_t fieldIndex = 1; fieldIndex <= fieldsTable.size(); ++fieldIndex) {
                const sol::object fieldObject = fieldsTable[fieldIndex];
                if (!fieldObject.valid() || !fieldObject.is<sol::table>()) {
                    error = "frame.fields 元素必须是 table";
                    return nullptr;
                }
                const auto fieldTable = fieldObject.as<sol::table>();
                StreamFieldDefinition field;
                field.name = fieldTable.get_or("name", std::string());
                if (field.name.empty()) {
                    error = "field.name 不能为空";
                    return nullptr;
                }

                const auto typeText = luaStringField(fieldTable, "type");
                if (!typeText.has_value()) {
                    error = "field.type 不能为空";
                    return nullptr;
                }
                const auto type = parseStreamValueType(*typeText);
                if (!type.has_value()) {
                    error = "未知字段类型: " + *typeText;
                    return nullptr;
                }
                field.type = *type;

                if (const auto offset = luaIntegerValue(fieldTable["offset"]); offset.has_value()) {
                    if (*offset <= 0) {
                        error = "field.offset 必须是从 1 开始的正整数";
                        return nullptr;
                    }
                    field.offset = static_cast<std::size_t>(*offset - 1);
                }

                const sol::object countObject = fieldTable["count"];
                if (countObject.valid() && countObject.get_type() != sol::type::lua_nil) {
                    const auto countPath = streamFieldCountPath(index - 1, fieldIndex - 1);
                    if (const auto count = luaIntegerValue(countObject); count.has_value()) {
                        if (*count < 0) {
                            error = countPath + " 不能为负数";
                            return nullptr;
                        }
                        field.count.fixed = static_cast<std::size_t>(*count);
                    } else if (countObject.is<std::string>()) {
                        field.count.fieldName = countObject.as<std::string>();
                    } else if (countObject.is<sol::protected_function>()) {
                        error = countPath
                              + " 检测到 function；该写法已废弃，不再支持 function。"
                                " 请迁移为 count 表达式 table，例如 count = { op = \"div\", field = \"byte_count\", by = 2 }";
                        return nullptr;
                    } else if (countObject.is<sol::table>()) {
                        field.count.expression = parseStreamCountExpressionObject(countObject, error);
                        if (!field.count.expression) {
                            error = countPath + " 表达式无效: " + error;
                            return nullptr;
                        }
                    } else {
                        error = countPath + " 仅支持整数、字段名或 count 表达式 table";
                        return nullptr;
                    }
                }

                frame.fields.push_back(std::move(field));
            }
        }

        const sol::object onFrameObject = frameTable["on_frame"];
        if (!onFrameObject.valid() || onFrameObject.get_type() == sol::type::lua_nil || !onFrameObject.is<sol::protected_function>()) {
            error = "frame.on_frame 必须是 function";
            return nullptr;
        }
        const auto callbackKey = frameOnFrameCallbackKey(frame.name);
        callbacks.insert_or_assign(callbackKey, onFrameObject.as<sol::protected_function>());
        loaded->frameCallbackKeys.insert_or_assign(frame.name, callbackKey);
        frames.push_back(std::move(frame));
    }

    if (const sol::object onErrorObject = schemaTable["on_error"]; onErrorObject.valid() && onErrorObject.get_type() != sol::type::lua_nil) {
        if (!onErrorObject.is<sol::protected_function>()) {
            error = "stream.on_error 必须是 function";
            return nullptr;
        }
        const std::string onErrorCallbackKey = "stream.on_error";
        callbacks.insert_or_assign(onErrorCallbackKey, onErrorObject.as<sol::protected_function>());
        loaded->onErrorCallbackKey = onErrorCallbackKey;
    }

    loaded->parser = FrameStreamParser(bufferDefinition, std::move(frames));
    return loaded;
}

} // namespace protoscope::scripting
