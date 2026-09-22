#pragma once

#include "protoscope/data/csv.hpp"
#include "protoscope/data/model.hpp"

namespace protoscope::data {

class RecordCsvWriter {
public:
    RecordCsvWriter(std::ostream& stream,std::map<std::uint64_t,Schema> schemas,std::stop_token stop={});
    void append(const Record& record);
    void finish();
private:
    std::ostream& stream_;
    std::map<std::uint64_t,Schema> schemas_;
    std::stop_token stop_;
    std::uint64_t count_{0};
    bool closed_{false};
};

class RecordCsvReader {
public:
    explicit RecordCsvReader(std::istream& stream,std::stop_token stop={});
    std::optional<Record> next();
    const std::map<std::uint64_t,Schema>& schemas() const {return schemas_;}
private:
    CsvRowReader rows_;
    std::map<std::uint64_t,Schema> schemas_;
    std::optional<std::vector<std::string>> pending_;
    std::uint64_t count_{0};
    bool finished_{false},failed_{false};
};

struct CsvImportMapping {
    Schema schema;
    std::string protocol;
    std::string device;
    std::string receivedColumn{"received_at_us"};
    std::optional<std::string> deviceColumn;
    std::optional<std::string> deviceTimeColumn;
    std::map<std::string,std::string> fields;
    // 普通 CSV 只有显式配置的标记才表示 null；空字符串默认仍是字符串。
    std::optional<std::string> nullToken;
};

class MappedCsvReader {
public:
    MappedCsvReader(std::istream& stream,CsvImportMapping mapping,std::stop_token stop={});
    std::optional<Record> next();
private:
    CsvRowReader rows_;
    CsvImportMapping mapping_;
    std::map<std::string,std::size_t> columns_;
    std::vector<std::size_t> fields_;
    bool failed_{false};
};

} // namespace protoscope::data
