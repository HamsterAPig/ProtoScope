#include "protoscope/data/query.hpp"

#include <algorithm>
#include <stdexcept>

namespace protoscope::data {
void validateConditions(const std::vector<FieldCondition>& conditions)
{
    if (conditions.size()>16) throw std::invalid_argument("query exceeds 16 field conditions");
    std::size_t bytes=0;
    for (const auto& condition:conditions) {
        if (condition.field.empty() || condition.field.size()>4096 || condition.field.find('\0')!=condition.field.npos ||
            condition.op<CompareOp::Equal || condition.op>CompareOp::NotNull || condition.value.value.index()>5)
            throw std::invalid_argument("invalid field condition");
        const bool unary=condition.op==CompareOp::IsNull || condition.op==CompareOp::NotNull;
        if (unary != std::holds_alternative<std::monostate>(condition.value.value))
            throw std::invalid_argument("null tests require is_null/not_null without a value");
        if (condition.op==CompareOp::Contains && !std::holds_alternative<std::string>(condition.value.value))
            throw std::invalid_argument("contains requires a string");
        bytes+=condition.field.size()+encodeValue(condition.value,{65536,1}).size();
        if (bytes>65536) throw std::invalid_argument("query conditions exceed 64 KiB");
    }
}

const Value* fieldValue(const Record& record, const Schema& schema, const std::string& field)
{
    const auto found=std::find_if(schema.fields.begin(),schema.fields.end(),[&](const auto& f){return f.name==field;});
    if (found==schema.fields.end()) return nullptr;
    const auto index=static_cast<std::size_t>(found-schema.fields.begin());
    return index<record.values.size() ? &record.values[index] : nullptr;
}

int compareValues(const Value& left, const Value& right)
{
    if (left.value.index()!=right.value.index()) return left.value.index()<right.value.index() ? -1:1;
    return std::visit([&](const auto& a) -> int {
        using T=std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T,std::monostate>) return 0;
        else if constexpr (std::is_same_v<T,Value::Array> || std::is_same_v<T,Value::Object>)
            throw std::invalid_argument("query values must be scalars");
        else {
            const auto& b=std::get<T>(right.value);
            return a<b ? -1 : a>b ? 1:0;
        }
    },left.value);
}

bool matches(const Record& record, const Schema& schema, const std::vector<FieldCondition>& conditions)
{
    for (const auto& condition:conditions) {
        const auto* value=fieldValue(record,schema,condition.field);
        // 旧模式没有该字段与显式 null 不同，不能将缺字段记录匹配为空值记录。
        if (!value) return false;
        const bool null=std::holds_alternative<std::monostate>(value->value);
        if (condition.op==CompareOp::IsNull) {if (!null) return false;continue;}
        if (condition.op==CompareOp::NotNull) {if (null) return false;continue;}
        if (value->value.index()!=condition.value.value.index()) return false;
        if (condition.op==CompareOp::Contains) {
            if (std::get<std::string>(value->value).find(std::get<std::string>(condition.value.value))==std::string::npos)
                return false;
            continue;
        }
        const int order=compareValues(*value,condition.value);
        if ((condition.op==CompareOp::Equal && order!=0) || (condition.op==CompareOp::NotEqual && order==0) ||
            (condition.op==CompareOp::Less && order>=0) || (condition.op==CompareOp::LessEqual && order>0) ||
            (condition.op==CompareOp::Greater && order<=0) || (condition.op==CompareOp::GreaterEqual && order<0))
            return false;
    }
    return true;
}

Value conditionsValue(const std::vector<FieldCondition>& conditions)
{
    validateConditions(conditions);
    Value::Array result;
    for (const auto& condition:conditions)
        result.push_back({Value::Array{{condition.field},{static_cast<std::int64_t>(condition.op)},condition.value}});
    return {std::move(result)};
}

std::vector<FieldCondition> conditionsFromValue(const Value& value)
{
    std::vector<FieldCondition> result;
    for (const auto& entry:std::get<Value::Array>(value.value)) {
        const auto& fields=std::get<Value::Array>(entry.value);
        if (fields.size()!=3) throw std::invalid_argument("invalid encoded condition");
        const auto operation=std::get<std::int64_t>(fields[1].value);
        if (operation<0 || operation>static_cast<std::int64_t>(CompareOp::NotNull))
            throw std::invalid_argument("invalid comparison operator");
        result.push_back({std::get<std::string>(fields[0].value),static_cast<CompareOp>(operation),fields[2]});
    }
    validateConditions(result);
    return result;
}
} // namespace protoscope::data
