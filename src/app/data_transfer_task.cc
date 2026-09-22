#include "protoscope/app/data_transfer_task.hpp"
#include "protoscope/plot/csv_data_file.hpp"
#include "protoscope/plot/data_file_output.hpp"

#include <algorithm>
#include <array>
#include <fstream>

namespace protoscope::app {

DataTransferTask::~DataTransferTask()
{
    cancel();
}

DataTransferStatus DataTransferTask::status() const
{
    std::lock_guard lock(mutex_);
    return status_;
}

void DataTransferTask::cancel()
{
    {
        std::lock_guard lock(mutex_);
        if (!status_.active) return;
        status_.canceled = true;
        batches_.clear();
        if (status_.complete) status_.active = false;
    }
    worker_.request_stop();
    changed_.notify_all();
}

void DataTransferTask::confirm()
{
    std::lock_guard lock(mutex_);
    confirmed_ = true;
    status_.awaitingConfirmation = false;
    changed_.notify_all();
}

void DataTransferTask::fail(std::string error)
{
    cancel();
    std::lock_guard lock(mutex_);
    status_.error = std::move(error);
}

bool DataTransferTask::push(DataImportBatch batch, std::stop_token stop)
{
    std::unique_lock lock(mutex_);
    // 四个批次形成背压，主循环按样本、字节和耗时预算消费。
    if (!changed_.wait(lock, stop, [&] { return batches_.size() < 4; })) return false;
    if (stop.stop_requested()) return false;
    batch.id = status_.id;
    batches_.push_back(std::move(batch));
    return true;
}

void DataTransferTask::finish(std::string error, bool canceled, bool committed)
{
    std::lock_guard lock(mutex_);
    if (status_.error.empty()) status_.error = std::move(error);
    status_.canceled = !committed && (canceled || status_.canceled);
    status_.complete = true;
    status_.awaitingConfirmation = false;
    if (status_.canceled || !status_.error.empty()) batches_.clear();
    status_.active = !batches_.empty();
}

std::optional<DataImportBatch> DataTransferTask::take(std::size_t maxSamples, std::size_t maxBytes)
{
    std::lock_guard lock(mutex_);
    if (batches_.empty()) return std::nullopt;
    std::size_t eventBytes = 0;
    for (const auto& event : batches_.front().events) eventBytes += (std::max<std::size_t>)(1, event.bytes.size());
    if (batches_.front().samples.size() > maxSamples ||
        eventBytes > maxBytes ||
        (batches_.front().event && batches_.front().event->bytes.size() > maxBytes)) return std::nullopt;
    auto batch = std::move(batches_.front());
    batches_.pop_front();
    if (batches_.empty() && status_.complete) status_.active = false;
    changed_.notify_all();
    return batch;
}

void DataTransferTask::submitted(std::size_t count)
{
    std::lock_guard lock(mutex_);
    status_.submitted += count;
}

bool DataTransferTask::startImport(const std::filesystem::path& path)
{
    if (status().active) return false;
    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard lock(mutex_);
        const auto id = status_.id + 1;
        status_ = {};
        status_.id = id;
        status_.active = true;
        status_.importing = true;
        confirmed_ = false;
        batches_.clear();
    }
    worker_ = std::jthread([this, path](std::stop_token stop) {
        plot::dataFileStopToken() = stop;
        try {
            std::string error;
            std::ifstream in(path, std::ios::binary);
            std::array<char, 4096> prefix{};
            in.read(prefix.data(), prefix.size());
            const std::string_view text(prefix.data(), static_cast<std::size_t>(in.gcount()));
            std::shared_ptr<session::SessionPackageData> package;
            DataImportBatch pending;
            std::size_t pendingBytes = 0;
            const auto flush = [&]() {
                if (pending.events.empty()) return true;
                const bool accepted = push(std::move(pending), stop);
                pending = {};
                pendingBytes = 0;
                return accepted;
            };
            const auto produced = [&](std::size_t count) {
                std::lock_guard lock(mutex_);
                status_.total += count;
            };
            plot::DataFileReadCallbacks callbacks;
            callbacks.metadata = [&](const plot::RawCaptureFileData& metadata, bool records) {
                {
                    std::unique_lock lock(mutex_);
                    status_.metadata = metadata;
                    status_.includesRecords = records || package != nullptr;
                    status_.awaitingConfirmation = true;
                    if (!changed_.wait(lock, stop, [&] { return confirmed_; })) return false;
                }
                return push({.metadata = metadata, .session = package}, stop);
            };
            callbacks.samples = [&](std::size_t channel, std::size_t offset, std::vector<plot::WaveSample> samples) {
                produced(samples.size());
                return push({.channel = channel, .sampleIndexOffset = offset, .samples = std::move(samples)}, stop);
            };
            callbacks.event = [&](plot::RawCaptureEvent event, bool continuation) {
                if (!continuation) produced(1);
                if (event.type != plot::RawCaptureEventType::RxBytes && event.type != plot::RawCaptureEventType::TxBytes) {
                    if (!flush()) return false;
                    return push({.event = std::move(event)}, stop);
                }
                if (event.bytes.size() > 65536 || continuation) {
                    if (!flush()) return false;
                    auto bytes = std::move(event.bytes);
                    for (std::size_t i = 0; i < bytes.size(); i += 65536) {
                        auto chunk = event;
                        const auto end = (std::min)(i + 65536, bytes.size());
                        chunk.bytes.assign(bytes.begin() + i, bytes.begin() + end);
                        if (!push({.event = std::move(chunk), .eventContinuation = continuation || i != 0}, stop))
                            return false;
                    }
                    return true;
                }
                const auto cost = (std::max<std::size_t>)(1, event.bytes.size());
                if (pendingBytes + cost > 65536 || pending.events.size() >= 256)
                    if (!flush()) return false;
                pendingBytes += cost;
                pending.events.push_back(std::move(event));
                return true;
            };
            if (text.starts_with("ProtoScopeSessionPackage")) {
                session::SessionRawCaptureSlice rawSlice;
                auto loaded = session::readSessionPackage(path, error, &rawSlice);
                if (!loaded) { finish(error, false); return; }
                package = std::make_shared<session::SessionPackageData>(std::move(*loaded));
                const auto* raw = session::findSessionPackageEntry(*package, "raw_capture.psraw");
                if (!raw) { finish("现场包缺少原始数据", false); return; }
                auto capture = plot::readRawCaptureFileRegion(path, rawSlice.offset, rawSlice.size, error, callbacks);
                const bool flushed = capture && flush();
                finish(std::move(error), !flushed && stop.stop_requested());
                return;
            } else if (text.starts_with("ProtoScopeRawCapture")) {
                const bool loaded = plot::readRawCaptureFile(path, error, &callbacks).has_value();
                const bool flushed = loaded && flush();
                finish(std::move(error), !flushed && stop.stop_requested());
                return;
            } else {
                const auto kind = plot::detectCsvKind(path, error);
                const bool loaded = kind == plot::CsvKind::RawEvents ?
                    plot::readRawCaptureCsvFile(path, error, &callbacks).has_value() :
                    kind == plot::CsvKind::Wave && plot::readWaveCsvFile(path, error, &callbacks).has_value();
                const bool flushed = loaded && flush();
                finish(std::move(error), !flushed && stop.stop_requested());
                return;
            }
        } catch (const std::exception& ex) {
            finish(ex.what(), stop.stop_requested());
        }
    });
    return true;
}

bool DataTransferTask::startExport(std::function<bool(std::stop_token, std::string&)> writer, std::size_t total)
{
    if (status().active) return false;
    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard lock(mutex_);
        const auto id = status_.id + 1;
        status_ = {};
        status_.id = id;
        status_.active = true;
        status_.total = total;
        batches_.clear();
    }
    worker_ = std::jthread([this, writer = std::move(writer)](std::stop_token stop) {
        plot::dataFileStopToken() = stop;
        // 编码数量批量汇报，避免百万样本逐条争用主线程状态锁。
        plot::dataFileProgressCallback() = [this, pending = std::size_t{0}](std::size_t count) mutable {
            pending += count;
            if (pending >= 1024) { submitted(pending); pending = 0; }
        };
        std::string error;
        bool committed = false;
        try {
            committed = writer(stop, error);
            if (!committed && error.empty() && !stop.stop_requested()) error = "数据导出失败";
        } catch (const std::exception& ex) { error = ex.what(); }
        if (committed) {
            std::lock_guard lock(mutex_);
            status_.submitted = status_.total;
        }
        finish(std::move(error), stop.stop_requested(), committed);
    });
    return true;
}

}
