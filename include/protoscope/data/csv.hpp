#pragma once

#include <istream>
#include <optional>
#include <ostream>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace protoscope::data {

// 与既有波形 CSV 使用同一转义规则，包含 # 的文本也引用，避免被当成元数据。
inline std::string csvEscape(std::string_view value)
{
    if (value.find_first_of(",\"\n\r#")==std::string_view::npos) return std::string(value);
    std::string out;
    out.reserve(value.size()+2);out.push_back('"');
    for (const auto ch:value) {if (ch=='"') out.push_back('"');out.push_back(ch);}
    out.push_back('"');
    return out;
}

class CsvRowReader {
public:
    explicit CsvRowReader(std::istream& stream,std::stop_token stop={},
                          std::size_t maxBytes=32U*1024U*1024U,std::size_t maxFields=4096);
    std::optional<std::vector<std::string>> next();
private:
    std::istream& stream_;
    std::stop_token stop_;
    std::size_t maxBytes_,maxFields_;
    std::string prefix_;
    std::size_t prefixOffset_{0};
    bool failed_{false};
};

void writeCsvRow(std::ostream& stream,const std::vector<std::string>& fields,std::stop_token stop={});

} // namespace protoscope::data
