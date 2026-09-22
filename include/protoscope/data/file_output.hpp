#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <stop_token>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace protoscope::data {

inline std::stop_token& dataFileStopToken()
{
    static thread_local std::stop_token token;
    return token;
}
inline std::function<void(std::size_t)>& dataFileProgressCallback()
{
    static thread_local std::function<void(std::size_t)> callback;
    return callback;
}
inline void reportDataFileProgress(std::size_t count)
{
    if (const auto& callback=dataFileProgressCallback()) callback(count);
}

// 同目录临时文件成功关闭后才替换目标，编码失败不能破坏已有现场文件。
class DataFileOutput {
public:
    explicit DataFileOutput(const std::filesystem::path& target,std::stop_token stop=dataFileStopToken())
        :target_(target),stop_(stop)
    {
        static std::atomic<std::uint64_t> sequence{0};
        temporary_=target;
        temporary_+=".tmp-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+
                    "-"+std::to_string(++sequence);
        stream.open(temporary_,std::ios::binary|std::ios::trunc);
    }
    ~DataFileOutput()
    {
        stream.close();
        std::error_code ignored;
        if (!committed_) std::filesystem::remove(temporary_,ignored);
    }
    bool commit(std::string& error,bool overwrite=true)
    {
        if (stop_.stop_requested()) {error="数据任务已取消";return false;}
        stream.flush();
        if (!stream.good()) {error="数据文件写入失败";return false;}
        stream.close();
        if (stream.fail()) {error="数据文件关闭失败";return false;}
#ifdef _WIN32
        const auto flags=MOVEFILE_WRITE_THROUGH|(overwrite ? MOVEFILE_REPLACE_EXISTING:0);
        if (!MoveFileExW(temporary_.c_str(),target_.c_str(),flags)) {
            error="数据文件替换失败: "+std::to_string(GetLastError());return false;
        }
#else
        std::error_code ec;
        if (overwrite) std::filesystem::rename(temporary_,target_,ec);
        else {
            // 硬链接原子创建目标，不能先 exists 再 rename 而覆盖竞争者的新文件。
            std::filesystem::create_hard_link(temporary_,target_,ec);
            if (!ec) {std::error_code ignored;std::filesystem::remove(temporary_,ignored);}
        }
        if (ec) {error="数据文件替换失败: "+ec.message();return false;}
#endif
        committed_=true;
        return true;
    }
    std::ofstream stream;
private:
    std::filesystem::path target_,temporary_;
    std::stop_token stop_;
    bool committed_{false};
};
} // namespace protoscope::data
