#include "protoscope/data/psrec.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace protoscope::data {
namespace {
constexpr ValueLimits limits{32U*1024U*1024U,16,131072};
constexpr std::array<std::uint8_t,12> header{'P','S','R','E','C','\r','\n',0x1A,1,0,0,0};
constexpr std::uint32_t metadataBlock=1, recordBlock=2, endBlock=3;
constexpr auto crcTable=[] {
    std::array<std::uint32_t,256> table{};
    for (std::uint32_t i=0;i<256;++i) {
        auto value=i;
        for (int bit=0;bit<8;++bit) value=(value>>1)^((value&1) ? 0xEDB88320U:0U);
        table[i]=value;
    }
    return table;
}();
void checkStop(std::stop_token stop)
{
    if (stop.stop_requested()) throw std::runtime_error("psrec task canceled");
}
void put(std::span<std::uint8_t> output,std::uint64_t value)
{
    for (auto& byte:output) {byte=static_cast<std::uint8_t>(value);value>>=8;}
}
std::uint64_t get(std::span<const std::uint8_t> input)
{
    std::uint64_t value=0;
    for (std::size_t i=0;i<input.size();++i) value|=static_cast<std::uint64_t>(input[i])<<(i*8);
    return value;
}
std::uint32_t crc(std::uint32_t value,std::span<const std::uint8_t> bytes,std::stop_token stop)
{
    for (std::size_t i=0;i<bytes.size();++i) {
        if ((i&0xFFFF)==0) checkStop(stop);
        value=crcTable[(value^bytes[i])&255]^(value>>8);
    }
    return value;
}
void write(std::ostream& stream,std::span<const std::uint8_t> bytes,std::stop_token stop)
{
    while (!bytes.empty()) {
        checkStop(stop);
        const auto chunk=std::min<std::size_t>(bytes.size(),65536);
        stream.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(chunk));
        if (!stream) throw std::runtime_error("psrec write failed");
        bytes=bytes.subspan(chunk);
    }
}
void read(std::istream& stream,std::span<std::uint8_t> bytes,std::stop_token stop)
{
    while (!bytes.empty()) {
        checkStop(stop);
        const auto chunk=std::min<std::size_t>(bytes.size(),65536);
        stream.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(chunk));
        if (!stream || stream.gcount()!=static_cast<std::streamsize>(chunk))
            throw std::runtime_error("psrec truncated or unreadable");
        bytes=bytes.subspan(chunk);
    }
}
void writeBlock(std::ostream& stream,std::uint32_t type,std::uint64_t sequence,
                std::span<const std::uint8_t> payload,std::stop_token stop)
{
    if (payload.size()>limits.maxBytes) throw std::invalid_argument("psrec block exceeds limit");
    std::array<std::uint8_t,16> prefix{};
    put(std::span(prefix).first(4),type);
    put(std::span(prefix).subspan(4,4),payload.size());
    put(std::span(prefix).subspan(8),sequence);
    std::array<std::uint8_t,4> checksum{};
    put(checksum,crc(crc(0xFFFFFFFFU,prefix,stop),payload,stop)^0xFFFFFFFFU);
    write(stream,prefix,stop);write(stream,payload,stop);write(stream,checksum,stop);
}
struct Block {std::uint32_t type;Bytes payload;};
Block readBlock(std::istream& stream,std::uint64_t sequence,std::stop_token stop)
{
    std::array<std::uint8_t,16> prefix{};
    read(stream,prefix,stop);
    const auto type=get(std::span(prefix).first(4));
    const auto size=get(std::span(prefix).subspan(4,4));
    if (get(std::span(prefix).subspan(8))!=sequence || size>limits.maxBytes ||
        type<metadataBlock || type>endBlock)
        throw std::invalid_argument("invalid psrec block header");
    Block block{static_cast<std::uint32_t>(type),Bytes(static_cast<std::size_t>(size))};
    read(stream,block.payload,stop);
    std::array<std::uint8_t,4> checksum{};
    read(stream,checksum,stop);
    if (get(checksum)!=(crc(crc(0xFFFFFFFFU,prefix,stop),block.payload,stop)^0xFFFFFFFFU))
        throw std::invalid_argument("psrec CRC32 mismatch");
    return block;
}
Bytes metadata(const std::map<std::uint64_t,Schema>& schemas,std::stop_token stop)
{
    if (schemas.size()>1024) throw std::invalid_argument("psrec schema count exceeds limit");
    Value::Array result;
    std::size_t budget=limits.maxBytes;
    std::size_t fields=0;
    for (const auto& [version,schema]:schemas) {
        checkStop(stop);
        if (!version || version>static_cast<std::uint64_t>(INT64_MAX))
            throw std::invalid_argument("invalid psrec schema version");
        fields+=schema.fields.size();
        if (fields>16384) throw std::invalid_argument("psrec total schema fields exceed limit");
        auto value=schemaValue(schema);
        const auto size=encodeValue(value,limits).size();
        if (size>budget) throw std::invalid_argument("psrec metadata exceeds limit");
        budget-=size;
        result.push_back({Value::Array{{static_cast<std::int64_t>(version)},std::move(value)}});
    }
    return encodeValue({std::move(result)},limits);
}
std::map<std::uint64_t,Schema> parseMetadata(const Bytes& payload)
{
    const auto decoded=decodeValue(payload,limits);
    const auto* entries=std::get_if<Value::Array>(&decoded.value);
    if (!entries || entries->size()>1024) throw std::invalid_argument("invalid psrec schemas");
    std::map<std::uint64_t,Schema> result;
    std::size_t fields=0;
    for (const auto& entry:*entries) {
        const auto* parts=std::get_if<Value::Array>(&entry.value);
        if (!parts || parts->size()!=2) throw std::invalid_argument("invalid psrec schema entry");
        const auto* version=std::get_if<std::int64_t>(&(*parts)[0].value);
        if (!version || *version<=0 ||
            !result.emplace(static_cast<std::uint64_t>(*version),schemaFromValue((*parts)[1])).second)
            throw std::invalid_argument("invalid or duplicate psrec schema version");
        fields+=result.at(static_cast<std::uint64_t>(*version)).fields.size();
        if (fields>16384) throw std::invalid_argument("psrec total schema fields exceed limit");
    }
    return result;
}
}

PsrecWriter::PsrecWriter(std::ostream& stream,std::map<std::uint64_t,Schema> schemas,std::stop_token stop)
    :stream_(stream),schemas_(std::move(schemas)),stop_(stop)
{
    checkStop(stop_);
    const auto payload=metadata(schemas_,stop_);
    write(stream_,header,stop_);
    writeBlock(stream_,metadataBlock,0,payload,stop_);
}
void PsrecWriter::append(const Record& record)
{
    if (failed_ || finished_) throw std::logic_error("psrec writer is closed or failed");
    try {
        checkStop(stop_);
        const auto schema=schemas_.find(record.schemaVersion);
        if (schema==schemas_.end()) throw std::invalid_argument("unknown psrec record schema");
        validateRecord(schema->second,record);
        std::size_t budget=limits.maxBytes;
        for (const auto& value:record.values) {
            const auto text=std::get_if<std::string>(&value.value);
            const auto bytes=std::get_if<Bytes>(&value.value);
            const auto size=text ? text->size():bytes ? bytes->size():sizeof(Value);
            if (size>budget) throw std::invalid_argument("psrec record exceeds limit");
            budget-=size;
        }
        if (count_==std::numeric_limits<std::uint64_t>::max()-1)
            throw std::overflow_error("psrec record count overflow");
        const auto payload=encodeValue(recordValue(record),limits);
        writeBlock(stream_,recordBlock,count_+1,payload,stop_);
        ++count_;
    } catch (...) {failed_=true;throw;}
}
void PsrecWriter::finish()
{
    if (failed_ || finished_) throw std::logic_error("psrec writer is closed or failed");
    try {
        std::array<std::uint8_t,8> payload{};
        put(payload,count_);
        writeBlock(stream_,endBlock,count_+1,payload,stop_);
        stream_.flush();
        if (!stream_) throw std::runtime_error("psrec flush failed");
        checkStop(stop_);
        finished_=true;
    } catch (...) {failed_=true;throw;}
}
PsrecReader::PsrecReader(std::istream& stream,std::stop_token stop):stream_(stream),stop_(stop)
{
    std::array<std::uint8_t,header.size()> bytes{};
    read(stream_,bytes,stop_);
    if (bytes!=header) throw std::invalid_argument("unsupported psrec signature or version");
    const auto block=readBlock(stream_,0,stop_);
    if (block.type!=metadataBlock) throw std::invalid_argument("psrec metadata missing");
    schemas_=parseMetadata(block.payload);
}
std::optional<Record> PsrecReader::next()
{
    if (failed_) throw std::logic_error("psrec reader has failed");
    if (finished_) return std::nullopt;
    try {
        const auto block=readBlock(stream_,count_+1,stop_);
        if (block.type==endBlock) {
            // 必须验证结束块和物理 EOF，不能把完整块边界处的截断当成成功导入。
            if (block.payload.size()!=8 || get(block.payload)!=count_ ||
                stream_.peek()!=std::char_traits<char>::eof() || stream_.bad())
                throw std::invalid_argument("invalid psrec footer or trailing data");
            checkStop(stop_);
            finished_=true;
            return std::nullopt;
        }
        if (block.type!=recordBlock) throw std::invalid_argument("unexpected psrec metadata block");
        auto record=recordFromValue(decodeValue(block.payload,limits));
        const auto schema=schemas_.find(record.schemaVersion);
        if (schema==schemas_.end()) throw std::invalid_argument("unknown psrec record schema");
        validateRecord(schema->second,record);
        if (count_==std::numeric_limits<std::uint64_t>::max()-1)
            throw std::overflow_error("psrec record count overflow");
        ++count_;
        return record;
    } catch (...) {failed_=true;throw;}
}
} // namespace protoscope::data
