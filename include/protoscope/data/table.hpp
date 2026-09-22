#pragma once

#include "protoscope/data/query.hpp"
#include <deque>
#include <memory>

namespace protoscope::data {
struct TableRow {
    std::uint64_t id{0};
    std::shared_ptr<const Record> record;
    std::shared_ptr<const Schema> schema;
};

struct TablePage {
    std::vector<TableRow> rows;
    std::size_t offset{0};
    std::size_t limit{200};
    bool more{false};
    bool loading{false};
    std::optional<std::int64_t> snapshot;
    std::uint64_t revision{0};
    std::string error;
};

struct TableView {
    std::optional<std::string> device;
    std::optional<std::int64_t> fromUs;
    std::optional<std::int64_t> toUs;
    std::vector<FieldCondition> conditions;
    std::optional<FieldSort> sort;
    std::size_t offset{0};
    std::size_t limit{200};
};

std::size_t recordMemoryBytes(const Record& record);
void validateTableView(const TableView& view);

// 实时列表有界保留，页只共享不可变行；历史数据不进入此缓冲区。
class LiveTable {
public:
    LiveTable(Schema schema, std::size_t maxRows=200, std::size_t maxBytes=4U*1024U*1024U);
    bool append(Record record);
    TablePage page(const TableView& view) const;
    std::size_t size() const { return rows_.size(); }
    std::size_t memoryBytes() const { return bytes_; }
private:
    struct Entry { TableRow row; std::size_t bytes; };
    std::shared_ptr<const Schema> schema_;
    std::deque<Entry> rows_;
    std::size_t maxRows_;
    std::size_t maxBytes_;
    std::size_t bytes_{0};
    std::uint64_t nextId_{1};
    std::uint64_t revision_{0};
    std::string error_;
};
} // namespace protoscope::data
