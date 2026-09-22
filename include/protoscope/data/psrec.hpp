#pragma once

#include "protoscope/data/model.hpp"

#include <istream>
#include <ostream>
#include <stop_token>

namespace protoscope::data {

// 交换格式只处理有界块；文件原子替换和导入暂存由存储任务负责。
class PsrecWriter {
public:
    PsrecWriter(std::ostream& stream, std::map<std::uint64_t, Schema> schemas,
                std::stop_token stop = {});
    void append(const Record& record);
    void finish();
    [[nodiscard]] std::uint64_t count() const { return count_; }

private:
    std::ostream& stream_;
    std::map<std::uint64_t, Schema> schemas_;
    std::stop_token stop_;
    std::uint64_t count_{0};
    bool finished_{false};
    bool failed_{false};
};

class PsrecReader {
public:
    explicit PsrecReader(std::istream& stream, std::stop_token stop = {});
    // 只有返回 nullopt 才表示完整文件已校验；此前的记录仅可写入不可见暂存区。
    std::optional<Record> next();
    [[nodiscard]] const std::map<std::uint64_t, Schema>& schemas() const { return schemas_; }
    [[nodiscard]] std::uint64_t count() const { return count_; }

private:
    std::istream& stream_;
    std::map<std::uint64_t, Schema> schemas_;
    std::stop_token stop_;
    std::uint64_t count_{0};
    bool finished_{false};
    bool failed_{false};
};

} // namespace protoscope::data
