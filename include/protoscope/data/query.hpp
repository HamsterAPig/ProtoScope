#pragma once

#include "protoscope/data/model.hpp"

namespace protoscope::data {
enum class CompareOp { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual, Contains, IsNull, NotNull };
struct FieldCondition {
    std::string field;
    CompareOp op{CompareOp::Equal};
    Value value;
};
struct FieldSort {
    std::string field;
    bool descending{false};
};

void validateConditions(const std::vector<FieldCondition>& conditions);
const Value* fieldValue(const Record& record, const Schema& schema, const std::string& field);
// 不同类型按固定类型标签排序，比较条件不进行字符串或数字类型的隐式转换。
int compareValues(const Value& left, const Value& right);
bool matches(const Record& record, const Schema& schema, const std::vector<FieldCondition>& conditions);
Value conditionsValue(const std::vector<FieldCondition>& conditions);
std::vector<FieldCondition> conditionsFromValue(const Value& value);
} // namespace protoscope::data
