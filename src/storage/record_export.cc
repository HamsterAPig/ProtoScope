#include "record_export.hpp"
#include "protoscope/data/file_output.hpp"
#include "protoscope/data/psrec.hpp"
#include "protoscope/data/record_csv.hpp"

#include <stdexcept>

namespace protoscope::storage {
std::uint64_t exportRecordFile(const std::filesystem::path& path,ExportFormat format,
    const std::map<std::uint64_t,data::Schema>& schemas,
    const std::function<std::optional<data::Record>()>& next,std::stop_token stop,ExportOptions options)
{
    data::DataFileOutput output(path,stop);
    if (!output.stream) throw std::runtime_error("cannot open record export temporary file");
    std::uint64_t count=0;
    const auto checkSize=[&] {
        const auto position=output.stream.tellp();
        if (position<0 || static_cast<std::uint64_t>(position)>options.maxBytes)
            throw std::runtime_error("record export exceeds max_write_file_size_bytes");
    };
    const auto consume=[&](auto& writer) {
        checkSize();
        // 查询游标逐条解码，导出不创建全历史数组；失败只留下不可见临时输出。
        while (auto record=next()) {
            if (stop.stop_requested()) throw std::runtime_error("record export canceled");
            writer.append(*record);checkSize();++count;
        }
        writer.finish();checkSize();
    };
    if (format==ExportFormat::Psrec) {data::PsrecWriter writer(output.stream,schemas,stop);consume(writer);}
    else if (format==ExportFormat::Csv) {data::RecordCsvWriter writer(output.stream,schemas,stop);consume(writer);}
    else throw std::invalid_argument("unsupported record export format");
    std::string error;
    if (!output.commit(error,options.overwrite)) throw std::runtime_error(error);
    return count;
}
}
