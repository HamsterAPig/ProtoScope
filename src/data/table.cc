#include "protoscope/data/table.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace protoscope::data {
std::size_t recordMemoryBytes(const Record& record)
{
    std::size_t bytes=sizeof(Record)+record.protocol.capacity()+record.dataset.capacity()+record.device.capacity()+
                      record.values.capacity()*sizeof(Value)+sizeof(TableRow)+64;
    for (const auto& field:record.values) {
        if (const auto* text=std::get_if<std::string>(&field.value)) bytes+=text->capacity();
        else if (const auto* data=std::get_if<Bytes>(&field.value)) bytes+=data->capacity();
        else if (field.value.index()>5) throw std::invalid_argument("table record requires scalar fields");
    }
    return bytes;
}

void validateTableView(const TableView& view)
{
    validateConditions(view.conditions);
    if (view.limit<1 || view.limit>1000 || view.offset>static_cast<std::size_t>(INT64_MAX) ||
        (view.fromUs && view.toUs && *view.fromUs>*view.toUs) ||
        (view.device && view.device->size()>4096) ||
        (view.sort && (view.sort->field.empty() || view.sort->field.size()>4096 ||
                       view.sort->field.find('\0')!=std::string::npos)))
        throw std::invalid_argument("invalid table view");
}

LiveTable::LiveTable(Schema schema,std::size_t maxRows,std::size_t maxBytes)
    : schema_(std::make_shared<const Schema>(std::move(schema))),maxRows_(maxRows),maxBytes_(maxBytes)
{
    validateSchema(*schema_);
    if (maxRows<1 || maxRows>1000 || maxBytes<1 || maxBytes>16U*1024U*1024U)
        throw std::invalid_argument("invalid live table limits");
}

bool LiveTable::append(Record record)
{
    validateRecord(*schema_,record);
    const auto bytes=recordMemoryBytes(record);
    ++revision_;
    if (bytes>maxBytes_) {
        error_="record exceeds live table memory limit";
        return false;
    }
    // 先构造新行，再淘汰旧行；保留中的 UI 页引用不随缓存滚动失效。
    auto shared=std::make_shared<const Record>(std::move(record));
    while (!rows_.empty() && (rows_.size()>=maxRows_ || bytes>maxBytes_-bytes_)) {
        bytes_-=rows_.front().bytes;rows_.pop_front();
    }
    rows_.push_back({{nextId_++,std::move(shared),schema_},bytes});
    bytes_+=bytes;
    error_.clear();
    return true;
}

TablePage LiveTable::page(const TableView& view) const
{
    validateTableView(view);
    std::vector<TableRow> matches;
    for (const auto& entry:rows_) {
        const auto& record=*entry.row.record;
        if ((view.device && *view.device!=record.device) || (view.fromUs && record.receivedAtUs<*view.fromUs) ||
            (view.toUs && record.receivedAtUs>*view.toUs) || !data::matches(record,*schema_,view.conditions)) continue;
        matches.push_back(entry.row);
    }
    std::sort(matches.begin(),matches.end(),[&](const auto& a,const auto& b) {
        if (view.sort) {
            const auto* left=fieldValue(*a.record,*a.schema,view.sort->field);
            const auto* right=fieldValue(*b.record,*b.schema,view.sort->field);
            const int order=!left ? (right ? -1:0) : !right ? 1 : compareValues(*left,*right);
            if (order) return view.sort->descending ? order>0:order<0;
        }
        if (a.record->receivedAtUs!=b.record->receivedAtUs) return a.record->receivedAtUs<b.record->receivedAtUs;
        return a.id<b.id;
    });
    TablePage page;
    page.offset=view.offset;page.limit=view.limit;page.revision=revision_;page.error=error_;
    const auto begin=std::min(view.offset,matches.size());
    const auto end=begin+std::min(view.limit,matches.size()-begin);
    page.rows.assign(matches.begin()+static_cast<std::ptrdiff_t>(begin),matches.begin()+static_cast<std::ptrdiff_t>(end));
    page.more=end<matches.size();
    return page;
}
} // namespace protoscope::data
