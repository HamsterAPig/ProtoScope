#include "protoscope/data/record_csv.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace protoscope::data {
namespace {
template<class T> T number(std::string_view text)
{
    T value{};
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
    if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size())
        throw std::invalid_argument("invalid CSV number");
    if constexpr (std::is_floating_point_v<T>)
        if (!std::isfinite(value)) throw std::invalid_argument("non-finite CSV number");
    return value;
}
const char* typeName(FieldType type)
{
    switch(type) {
    case FieldType::Int64:return "int64";
    case FieldType::Double:return "double";
    case FieldType::Bool:return "bool";
    case FieldType::String:return "string";
    case FieldType::Bytes:return "bytes";
    }
    throw std::invalid_argument("invalid CSV field type");
}
FieldType fieldType(std::string_view name)
{
    for (const auto type:{FieldType::Int64,FieldType::Double,FieldType::Bool,FieldType::String,FieldType::Bytes})
        if (name==typeName(type)) return type;
    throw std::invalid_argument("unknown CSV field type");
}
Value parseScalar(FieldType type,std::string_view text)
{
    switch(type) {
    case FieldType::Int64:return {number<std::int64_t>(text)};
    case FieldType::Double:return {number<double>(text)};
    case FieldType::Bool:
        if (text=="true") return {true};
        if (text=="false") return {false};
        throw std::invalid_argument("CSV bool requires true/false");
    case FieldType::String:return {std::string(text)};
    case FieldType::Bytes: {
        if (text.size()%2) throw std::invalid_argument("odd CSV HEX length");
        const auto nibble=[](char ch) {
            if (ch>='0' && ch<='9') return ch-'0';
            if (ch>='a' && ch<='f') return ch-'a'+10;
            if (ch>='A' && ch<='F') return ch-'A'+10;
            throw std::invalid_argument("invalid CSV HEX digit");
        };
        Bytes bytes;bytes.reserve(text.size()/2);
        for (std::size_t i=0;i<text.size();i+=2)
            bytes.push_back(static_cast<std::uint8_t>(nibble(text[i])*16+nibble(text[i+1])));
        return {std::move(bytes)};
    }
    }
    throw std::invalid_argument("unsupported CSV scalar");
}
std::string scalarText(const Value& value)
{
    return std::visit([](const auto& item)->std::string {
        using T=std::decay_t<decltype(item)>;
        if constexpr(std::is_same_v<T,std::monostate>) return "n:";
        else if constexpr(std::is_same_v<T,std::int64_t>) return "i:"+std::to_string(item);
        else if constexpr(std::is_same_v<T,double>) {
            char buffer[64];
            const auto result=std::to_chars(buffer,buffer+sizeof(buffer),item,std::chars_format::general,
                                             std::numeric_limits<double>::max_digits10);
            if (result.ec!=std::errc{}) throw std::invalid_argument("CSV double formatting failed");
            return "d:"+std::string(buffer,result.ptr);
        } else if constexpr(std::is_same_v<T,bool>) return item ? "b:true":"b:false";
        else if constexpr(std::is_same_v<T,std::string>) return "s:"+item;
        else if constexpr(std::is_same_v<T,Bytes>) {
            constexpr char hex[]="0123456789ABCDEF";
            if (item.size()>16U*1024U*1024U) throw std::invalid_argument("CSV bytes exceed row limit");
            std::string text="x:";text.reserve(2+item.size()*2);
            for (const auto byte:item) {text+=hex[byte>>4];text+=hex[byte&15];}
            return text;
        } else throw std::invalid_argument("CSV requires scalar fields");
    },value.value);
}
Value typedScalar(const Field& field,std::string_view text)
{
    if (text=="n:") return {};
    constexpr char tags[]={'n','i','d','b','s','x'};
    if (text.size()<2 || text[1]!=':' || text[0]!=tags[static_cast<std::size_t>(field.type)])
        throw std::invalid_argument("CSV value tag differs from schema");
    return parseScalar(field.type,text.substr(2));
}
void stripBom(std::vector<std::string>& row)
{
    if (!row.empty() && row[0].starts_with("\xEF\xBB\xBF")) row[0].erase(0,3);
}
std::uint64_t version(const std::string& text)
{
    const auto value=number<std::int64_t>(text);
    if (value<=0) throw std::invalid_argument("CSV schema version must be positive");
    return static_cast<std::uint64_t>(value);
}
}
RecordCsvWriter::RecordCsvWriter(std::ostream& stream,std::map<std::uint64_t,Schema> schemas,std::stop_token stop)
    :stream_(stream),schemas_(std::move(schemas)),stop_(stop)
{
    if (schemas_.size()>1024) throw std::invalid_argument("CSV schema count exceeded");
    std::size_t fields=0;
    for (const auto& [id,schema]:schemas_) {
        if (!id || id>static_cast<std::uint64_t>(INT64_MAX)) throw std::invalid_argument("invalid CSV schema version");
        validateSchema(schema);fields+=schema.fields.size();
        if (fields>16384) throw std::invalid_argument("CSV total schema fields exceeded");
    }
    writeCsvRow(stream_,{"protoscope_csv","1"},stop_);
    for (const auto& [id,schema]:schemas_) {
        std::vector<std::string> row{"schema",std::to_string(id),schema.dataset};
        for (const auto& field:schema.fields) {
            row.push_back(field.name);row.emplace_back(typeName(field.type));
            row.emplace_back(field.nullable ? "true":"false");
        }
        writeCsvRow(stream_,row,stop_);
    }
}
void RecordCsvWriter::append(const Record& record)
{
    if (closed_) throw std::logic_error("CSV writer closed or failed");
    try {
        const auto schema=schemas_.find(record.schemaVersion);
        if (schema==schemas_.end()) throw std::invalid_argument("unknown CSV schema");
        validateRecord(schema->second,record);
        std::size_t budget=32U*1024U*1024U;
        for (const auto& value:record.values) {
            const auto text=std::get_if<std::string>(&value.value);
            const auto bytes=std::get_if<Bytes>(&value.value);
            const auto size=text ? text->size():bytes ? bytes->size():sizeof(Value);
            if (size>budget) throw std::invalid_argument("CSV record exceeds memory budget");
            budget-=size;
        }
        std::vector<std::string> row{"record",record.protocol,record.dataset,record.device,
            std::to_string(record.receivedAtUs),record.deviceTimeUs ? std::to_string(*record.deviceTimeUs):"",
            std::to_string(record.schemaVersion)};
        for (const auto& value:record.values) row.push_back(scalarText(value));
        if (count_==UINT64_MAX) throw std::overflow_error("CSV record count overflow");
        writeCsvRow(stream_,row,stop_);++count_;
    } catch (...) {closed_=true;throw;}
}
void RecordCsvWriter::finish()
{
    if (closed_) throw std::logic_error("CSV writer closed or failed");
    closed_=true;
    writeCsvRow(stream_,{"end",std::to_string(count_)},stop_);
    stream_.flush();
    if (!stream_) throw std::runtime_error("CSV flush failed");
    if (stop_.stop_requested()) throw std::runtime_error("CSV task canceled");
}
RecordCsvReader::RecordCsvReader(std::istream& stream,std::stop_token stop):rows_(stream,stop)
{
    auto header=rows_.next();
    if (header) stripBom(*header);
    if (!header || *header!=std::vector<std::string>{"protoscope_csv","1"})
        throw std::invalid_argument("unsupported record CSV version");
    std::size_t fields=0;
    for (;;) {
        auto row=rows_.next();
        if (!row) throw std::invalid_argument("CSV missing end row");
        if (row->empty() || (*row)[0]!="schema") {pending_=std::move(row);break;}
        if (row->size()<6 || (row->size()-3)%3) throw std::invalid_argument("invalid CSV schema row");
        Schema schema{(*row)[2],{}};
        for (std::size_t i=3;i<row->size();i+=3) {
            if ((*row)[i+2]!="true" && (*row)[i+2]!="false") throw std::invalid_argument("invalid CSV nullable flag");
            schema.fields.push_back({(*row)[i],fieldType((*row)[i+1]),(*row)[i+2]=="true"});
        }
        validateSchema(schema);fields+=schema.fields.size();
        if (schemas_.size()>=1024 || fields>16384 ||
            !schemas_.emplace(version((*row)[1]),std::move(schema)).second)
            throw std::invalid_argument("duplicate or excessive CSV schemas");
    }
}
std::optional<Record> RecordCsvReader::next()
{
    if (failed_) throw std::logic_error("CSV reader failed");
    if (finished_) return std::nullopt;
    try {
        auto row=pending_ ? std::move(pending_):rows_.next();
        pending_.reset();
        if (!row || row->empty()) throw std::invalid_argument("CSV missing end row");
        if ((*row)[0]=="end") {
            if (row->size()!=2 || number<std::uint64_t>((*row)[1])!=count_ || rows_.next())
                throw std::invalid_argument("invalid CSV footer or trailing rows");
            finished_=true;return std::nullopt;
        }
        if (row->size()<7 || (*row)[0]!="record") throw std::invalid_argument("invalid CSV record row");
        const auto id=version((*row)[6]);
        const auto schema=schemas_.find(id);
        if (schema==schemas_.end() || row->size()!=7+schema->second.fields.size())
            throw std::invalid_argument("CSV record columns differ from schema");
        Record record{(*row)[1],(*row)[2],(*row)[3],number<std::int64_t>((*row)[4]),{},id,{}};
        if (!(*row)[5].empty()) record.deviceTimeUs=number<std::int64_t>((*row)[5]);
        for (std::size_t i=0;i<schema->second.fields.size();++i)
            record.values.push_back(typedScalar(schema->second.fields[i],(*row)[i+7]));
        validateRecord(schema->second,record);
        if (count_==UINT64_MAX) throw std::overflow_error("CSV record count overflow");
        ++count_;return record;
    } catch (...) {failed_=true;throw;}
}
MappedCsvReader::MappedCsvReader(std::istream& stream,CsvImportMapping mapping,std::stop_token stop)
    :rows_(stream,stop),mapping_(std::move(mapping))
{
    validateSchema(mapping_.schema);
    if (mapping_.protocol.empty() || mapping_.protocol.size()>4096 || mapping_.device.size()>4096)
        throw std::invalid_argument("invalid CSV protocol/device");
    auto header=rows_.next();
    if (!header) throw std::invalid_argument("CSV header missing");
    stripBom(*header);
    for (std::size_t i=0;i<header->size();++i)
        if ((*header)[i].empty() || !columns_.emplace((*header)[i],i).second)
            throw std::invalid_argument("CSV empty or duplicate column name");
    for (const auto& [field,column]:mapping_.fields) {
        if (std::none_of(mapping_.schema.fields.begin(),mapping_.schema.fields.end(),
                        [&](const auto& item){return item.name==field;}))
            throw std::invalid_argument("CSV mapping refers to undeclared field");
    }
    // 显式映射的每个声明字段都必须存在；不以旧值或默认值补齐缺失列。
    columns_.at(mapping_.receivedColumn);
    if (mapping_.deviceColumn) columns_.at(*mapping_.deviceColumn);
    if (mapping_.deviceTimeColumn) columns_.at(*mapping_.deviceTimeColumn);
    for (const auto& field:mapping_.schema.fields) {
        const auto mapped=mapping_.fields.find(field.name);
        fields_.push_back(columns_.at(mapped==mapping_.fields.end() ? field.name:mapped->second));
    }
}
std::optional<Record> MappedCsvReader::next()
{
    if (failed_) throw std::logic_error("mapped CSV reader failed");
    try {
        const auto row=rows_.next();
        if (!row) return std::nullopt;
        if (row->size()!=columns_.size()) throw std::invalid_argument("CSV row column count mismatch");
        const auto cell=[&](const std::string& name)->const std::string& {return row->at(columns_.at(name));};
        Record record{mapping_.protocol,mapping_.schema.dataset,
            mapping_.deviceColumn ? cell(*mapping_.deviceColumn):mapping_.device,
            number<std::int64_t>(cell(mapping_.receivedColumn)),{},1,{}};
        if (mapping_.deviceTimeColumn) {
            const auto& text=cell(*mapping_.deviceTimeColumn);
            if (!text.empty() && (!mapping_.nullToken || text!=*mapping_.nullToken))
                record.deviceTimeUs=number<std::int64_t>(text);
        }
        for (std::size_t i=0;i<fields_.size();++i) {
            const auto& text=row->at(fields_[i]);
            record.values.push_back(mapping_.nullToken && text==*mapping_.nullToken ?
                Value{}:parseScalar(mapping_.schema.fields[i].type,text));
        }
        validateRecord(mapping_.schema,record);
        return record;
    } catch (...) {failed_=true;throw;}
}
} // namespace protoscope::data
