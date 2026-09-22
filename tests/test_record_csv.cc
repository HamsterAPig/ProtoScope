#include "protoscope/data/record_csv.hpp"
#include "protoscope/data/file_output.hpp"
#include "test_helpers.hpp"

#include <iostream>
#include <sstream>

namespace {
using namespace protoscope;
using tests::require;
template<class F> void rejects(F&& action,const char* message)
{
    bool rejected=false;
    try {action();} catch(const std::exception&) {rejected=true;}
    require(rejected,message);
}
void rows()
{
    const std::vector<std::string> expected{"",",\"#\r\n",std::string("a\0b",3),"tail"};
    std::ostringstream output;
    data::writeCsvRow(output,expected);
    std::istringstream input(output.str());
    data::CsvRowReader reader(input);
    require(reader.next()==expected && !reader.next(),"RFC quoting and binary text roundtrip");
    for (const auto* bad:{"a\"b\n","\"a\"suffix\n","\"unclosed"}) {
        std::istringstream stream(bad);
        data::CsvRowReader invalid(stream);
        rejects([&]{invalid.next();},"strict malformed quote rejected");
    }
    std::istringstream trailing("a,\r\n\n");
    data::CsvRowReader empty(trailing);
    require(empty.next()==std::vector<std::string>{"a",""},"trailing empty field");
    require(empty.next()==std::vector<std::string>{""},"blank row retained for strict caller validation");
    std::istringstream excessive("a,b,c");
    data::CsvRowReader limited(excessive,{},32,2);
    rejects([&]{limited.next();},"column bound");
    std::istringstream big("123456789");
    data::CsvRowReader small(big,{},8);
    rejects([&]{small.next();},"row byte bound");
}
void typed()
{
    const data::Schema schema{"samples",{
        {"i",data::FieldType::Int64,false},{"d",data::FieldType::Double,false},
        {"b",data::FieldType::Bool,false},{"s",data::FieldType::String,false},
        {"x",data::FieldType::Bytes,false},{"n",data::FieldType::String,true}}};
    const data::Record record{"proto","samples","dev",-1,std::int64_t{0},3,{
        {INT64_MAX},{-0.0},{false},{std::string("n:,\r\n\0\"",8)},{data::Bytes{0,255}},data::Value{}}};
    std::ostringstream output;
    data::RecordCsvWriter writer(output,{{3,schema}});
    writer.append(record);writer.append(record);writer.finish();
    std::istringstream input(output.str());
    data::RecordCsvReader reader(input);
    require(reader.schemas().at(3)==schema,"CSV schema metadata");
    for (int i=0;i<2;++i) {
        const auto restored=reader.next();
        require(restored && data::encodeValue(data::recordValue(*restored))==
                data::encodeValue(data::recordValue(record)),"CSV exact typed roundtrip");
    }
    require(!reader.next() && !reader.next(),"CSV complete footer");
    auto missing=output.str();missing.resize(missing.rfind("end,"));
    std::istringstream incomplete(missing);
    data::RecordCsvReader partial(incomplete);
    partial.next();partial.next();
    rejects([&]{partial.next();},"missing footer rejected");
    auto mismatch=output.str();mismatch.replace(mismatch.find("i:922"),2,"d:");
    std::istringstream mismatched(mismatch);
    data::RecordCsvReader invalid(mismatched);
    rejects([&]{invalid.next();},"type tag mismatch rejected");
}
data::CsvImportMapping mapping()
{
    data::CsvImportMapping result;
    result.schema={"sample",{{"counter",data::FieldType::Int64,false},{"text",data::FieldType::String,true}}};
    result.protocol="proto";result.device="device";
    result.fields={{"counter","n"}};result.nullToken="\\N";
    return result;
}
void mapped()
{
    std::istringstream input("\xEF\xBB\xBF\"received_at_us\",text,n\r\n10,,9223372036854775807\r\n11,\\N,2");
    data::MappedCsvReader reader(input,mapping());
    const auto first=reader.next(),second=reader.next();
    require(first && first->receivedAtUs==10 && std::get<std::int64_t>(first->values[0].value)==INT64_MAX &&
            std::get<std::string>(first->values[1].value).empty(),"mapped empty text distinct from null");
    require(second && second->values[1].value.index()==0 && !reader.next(),"explicit mapped null token");
    for (const auto* bad:{
        "received_at_us,text,n\n1,x,1.5",
        "received_at_us,text,n\n1,x,9223372036854775808",
        "received_at_us,text,n\n1,x,\\N",
        "received_at_us,text,n\n1,x",
        "received_at_us,text,n\n1,x,2,extra",
        "received_at_us,text,text\n1,x,y"}) {
        rejects([&]{std::istringstream stream(bad);data::MappedCsvReader invalid(stream,mapping());invalid.next();},
                "invalid ordinary CSV rejected");
    }
}
void cancellation()
{
    std::stop_source stop;stop.request_stop();
    std::istringstream input("a\n");
    data::CsvRowReader reader(input,stop.get_token());
    rejects([&]{reader.next();},"CSV read canceled");
    std::ostringstream output;
    rejects([&]{data::writeCsvRow(output,{"a"},stop.get_token());},"CSV write canceled");
    std::istringstream failure("a");
    failure.setstate(std::ios::badbit);
    data::CsvRowReader broken(failure);
    rejects([&]{broken.next();},"I/O failure is not EOF");
}
void atomicOutput()
{
    tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-record-csv"));
    const auto target=directory.path()/"result.csv";
    {std::ofstream old(target);old<<"original";}
    std::stop_source stop;
    {
        data::DataFileOutput output(target,stop.get_token());
        data::RecordCsvWriter writer(output.stream,{});
        writer.finish();stop.request_stop();
        std::string error;
        require(!output.commit(error) && !error.empty(),"canceled file must not replace existing target");
    }
    {std::ifstream old(target);std::string content;old>>content;require(content=="original","original target retained");}
    {
        data::DataFileOutput output(target);
        data::RecordCsvWriter writer(output.stream,{});
        writer.finish();
        std::string error;require(output.commit(error),"atomic CSV replacement");
    }
    std::ifstream input(target);
    data::RecordCsvReader reader(input);
    require(!reader.next(),"committed CSV readable");
    std::size_t files=0;
    for (const auto& entry:std::filesystem::directory_iterator(directory.path())) {
        require(entry.path()==target,"temporary output removed");++files;
    }
    require(files==1,"only committed target remains");
}
}
int main()
{
    int failed=0;
    for (const auto& [name,run]:std::initializer_list<std::pair<const char*,void(*)()>>{
        {"rows",rows},{"typed",typed},{"mapped",mapped},{"cancellation",cancellation},{"atomic_output",atomicOutput}}) {
        try {run();std::cout<<"[PASS] "<<name<<'\n';}
        catch(const std::exception& e){++failed;std::cerr<<"[FAIL] "<<name<<": "<<e.what()<<'\n';}
    }
    return failed?1:0;
}
