#include "protoscope/data/psrec.hpp"
#include "test_helpers.hpp"

#include <bit>
#include <iostream>
#include <limits>
#include <sstream>

namespace {
using namespace protoscope;
using tests::require;
const data::Schema schema{"sample",{
    {"integer",data::FieldType::Int64,false},{"number",data::FieldType::Double,false},
    {"boolean",data::FieldType::Bool,false},{"text",data::FieldType::String,false},
    {"bytes",data::FieldType::Bytes,false},{"empty",data::FieldType::String,true}}};
const data::Record record{"proto","sample","device",-123,std::int64_t{456},7,{
    {std::numeric_limits<std::int64_t>::min()},{-0.0},{true},{std::string("a\0\r\n,b",6)},
    {data::Bytes{0,1,255}},data::Value{}}};
template<class F> void rejects(F&& action,const char* message)
{
    bool rejected=false;
    try { action(); } catch (const std::exception&) { rejected=true; }
    require(rejected,message);
}
std::string encoded()
{
    std::ostringstream output(std::ios::binary);
    data::PsrecWriter writer(output,{{7,schema}});
    writer.append(record);writer.append(record);writer.finish();
    require(writer.count()==2,"writer count");
    return output.str();
}
void consume(const std::string& bytes)
{
    std::istringstream input(bytes,std::ios::binary);
    data::PsrecReader reader(input);
    while(reader.next()) {}
}
void roundtrip()
{
    std::istringstream input(encoded(),std::ios::binary);
    data::PsrecReader reader(input);
    require(reader.schemas().at(7)==schema,"schema exact roundtrip");
    for (int i=0;i<2;++i) {
        const auto restored=reader.next();
        require(restored && data::encodeValue(data::recordValue(*restored))==
                data::encodeValue(data::recordValue(record)),"typed values preserve exact bits");
    }
    require(!reader.next() && !reader.next() && reader.count()==2,"footer and idempotent EOF");
    std::ostringstream empty;
    data::PsrecWriter writer(empty,{});writer.finish();consume(empty.str());
    rejects([&]{writer.append(record);},"no append after finish");
    rejects([&]{writer.finish();},"no duplicate footer");
}
void corruption()
{
    const auto original=encoded();
    for (std::size_t size=0;size<original.size();++size)
        rejects([&]{consume(original.substr(0,size));},"all truncated prefixes rejected");
    for (std::size_t offset=0;offset<original.size();++offset) {
        auto bytes=original;bytes[offset]^=1;
        rejects([&]{consume(bytes);},"all single-byte corruptions rejected");
    }
    rejects([&]{consume(original+"x");},"trailing bytes rejected");
    rejects([&]{consume(original+original);},"concatenated archives rejected");
}
void validationAndFailure()
{
    const data::Value smallTags{data::Value::Array(1000)};
    const auto tags=data::encodeValue(smallTags);
    rejects([&]{data::decodeValue(tags,{4096,16,100});},"decoder node amplification bounded");
    rejects([&]{data::encodeValue(smallTags,{4096,16,100});},"encoder shares node bound");
    std::ostringstream output;
    data::PsrecWriter writer(output,{{7,schema}});
    auto invalid=record;invalid.schemaVersion=8;
    rejects([&]{writer.append(invalid);},"unknown schema rejected");
    rejects([&]{writer.finish();},"failed archive cannot be finalized");
    std::ostringstream bad;
    bad.setstate(std::ios::badbit);
    rejects([&]{data::PsrecWriter failed(bad,{});},"output failure detected");
    std::ostringstream tooMany;
    std::map<std::uint64_t,data::Schema> schemas;
    for (std::uint64_t i=1;i<=1025;++i) schemas.emplace(i,schema);
    rejects([&]{data::PsrecWriter failed(tooMany,schemas);},"schema count bounded");
    auto damaged=encoded();damaged.back()^=1;
    std::istringstream input(damaged);
    data::PsrecReader reader(input);
    reader.next();reader.next();
    rejects([&]{reader.next();},"footer corruption rejected");
    rejects([&]{reader.next();},"failed reader cannot resume");
}
void cancellation()
{
    std::stop_source stop;
    std::ostringstream output;
    data::PsrecWriter writer(output,{{7,schema}},stop.get_token());
    writer.append(record);stop.request_stop();
    rejects([&]{writer.finish();},"canceled output not finalized");
    std::istringstream input(encoded());
    std::stop_source reading;
    data::PsrecReader reader(input,reading.get_token());
    require(reader.next().has_value(),"read before cancellation");
    reading.request_stop();
    rejects([&]{reader.next();},"read cancellation");
}
}
int main()
{
    int failed=0;
    for (const auto& [name,run]:std::initializer_list<std::pair<const char*,void(*)()>>{
        {"roundtrip",roundtrip},{"corruption",corruption},
        {"validation_failure",validationAndFailure},{"cancellation",cancellation}}) {
        try {run();std::cout<<"[PASS] "<<name<<'\n';}
        catch(const std::exception& error){++failed;std::cerr<<"[FAIL] "<<name<<": "<<error.what()<<'\n';}
    }
    return failed?1:0;
}
