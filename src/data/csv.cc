#include "protoscope/data/csv.hpp"

#include <stdexcept>
#include <algorithm>

namespace protoscope::data {
namespace {
void checkStop(std::stop_token stop)
{
    if (stop.stop_requested()) throw std::runtime_error("CSV task canceled");
}
}
CsvRowReader::CsvRowReader(std::istream& stream,std::stop_token stop,std::size_t maxBytes,std::size_t maxFields)
    :stream_(stream),stop_(stop),maxBytes_(maxBytes),maxFields_(maxFields)
{
    if (!maxBytes || !maxFields) throw std::invalid_argument("invalid CSV limits");
    // BOM 位于 CSV 语法之外；非 BOM 的同前缀 UTF-8 字节仍交还字段解析器。
    if (stream_.peek()==0xEF) {
        for (int i=0;i<3;++i) {
            const auto ch=stream_.get();
            if (ch==std::char_traits<char>::eof()) break;
            prefix_.push_back(static_cast<char>(ch));
        }
        if (prefix_=="\xEF\xBB\xBF") prefix_.clear();
    }
}
std::optional<std::vector<std::string>> CsvRowReader::next()
{
    if (failed_) throw std::logic_error("CSV reader has failed");
    try {
        enum class State {Start,Unquoted,Quoted,Closed};
        State state=State::Start;
        std::vector<std::string> fields;
        std::string value;
        std::size_t bytes=0;
        auto finish=[&] {
            if (fields.size()>=maxFields_) throw std::invalid_argument("CSV column limit exceeded");
            fields.push_back(std::move(value));value.clear();state=State::Start;
        };
        for (;;) {
            if ((bytes&0xFFFF)==0) checkStop(stop_);
            const auto next=prefixOffset_<prefix_.size() ?
                static_cast<int>(static_cast<unsigned char>(prefix_[prefixOffset_++])):stream_.get();
            if (next==std::char_traits<char>::eof()) {
                if (stream_.bad() || (!stream_.eof() && stream_.fail())) throw std::runtime_error("CSV read failed");
                if (state==State::Quoted) throw std::invalid_argument("CSV unclosed quote");
                if (!bytes) return std::nullopt;
                finish();return fields;
            }
            if (++bytes>maxBytes_) throw std::invalid_argument("CSV row byte limit exceeded");
            const char ch=static_cast<char>(next);
            if (state==State::Quoted) {
                if (ch=='"') state=State::Closed;
                else value.push_back(ch);
                continue;
            }
            if (state==State::Closed && ch=='"') {value.push_back(ch);state=State::Quoted;continue;}
            if (ch==',') {finish();continue;}
            if (ch=='\n' || ch=='\r') {
                if (ch=='\r' && stream_.peek()=='\n') stream_.get();
                finish();return fields;
            }
            // 引号仅能开始字段；闭引号后只能是转义引号或字段/行分隔符。
            if (state==State::Closed || (ch=='"' && state!=State::Start))
                throw std::invalid_argument("CSV misplaced quote");
            if (ch=='"') state=State::Quoted;
            else {state=State::Unquoted;value.push_back(ch);}
        }
    } catch (...) {failed_=true;throw;}
}
void writeCsvRow(std::ostream& stream,const std::vector<std::string>& fields,std::stop_token stop)
{
    checkStop(stop);
    std::size_t bytes=0;
    if (fields.size()>4096) throw std::invalid_argument("CSV column limit exceeded");
    for (const auto& field:fields) {
        if (field.size()>32U*1024U*1024U-bytes) throw std::invalid_argument("CSV row byte limit exceeded");
        bytes+=field.size();
    }
    bytes=1;
    for (std::size_t i=0;i<fields.size();++i) {
        checkStop(stop);
        if (i) stream.put(',');
        const auto escaped=csvEscape(fields[i]);
        if (escaped.size()+(i ? 1:0)>32U*1024U*1024U-bytes)
            throw std::invalid_argument("CSV encoded row exceeds limit");
        bytes+=escaped.size()+(i ? 1:0);
        for (std::size_t offset=0;offset<escaped.size();offset+=65536) {
            checkStop(stop);
            const auto count=std::min<std::size_t>(65536,escaped.size()-offset);
            stream.write(escaped.data()+offset,static_cast<std::streamsize>(count));
            if (!stream) throw std::runtime_error("CSV write failed");
        }
    }
    stream.put('\n');
    if (!stream) throw std::runtime_error("CSV write failed");
}
} // namespace protoscope::data
