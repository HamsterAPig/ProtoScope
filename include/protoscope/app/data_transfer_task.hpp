#pragma once

#include "protoscope/plot/raw_capture_file.hpp"
#include "protoscope/session/session_package.hpp"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>

namespace protoscope::app {

struct DataTransferStatus {
    std::uint64_t id{0};
    bool active{false};
    bool awaitingConfirmation{false};
    bool canceled{false};
    bool complete{false};
    bool importing{false};
    bool includesRecords{false};
    std::size_t submitted{0};
    std::size_t total{0};
    std::string error;
    plot::RawCaptureFileData metadata;
};

struct DataImportBatch {
    std::uint64_t id{0};
    std::optional<plot::RawCaptureFileData> metadata;
    std::size_t channel{0};
    std::size_t sampleIndexOffset{0};
    std::vector<plot::WaveSample> samples;
    std::optional<plot::RawCaptureEvent> event;
    bool eventContinuation{false};
    std::shared_ptr<session::SessionPackageData> session;
    std::vector<plot::RawCaptureEvent> events;
};

class DataTransferTask {
public:
    ~DataTransferTask();
    bool startImport(const std::filesystem::path& path);
    bool startExport(std::function<bool(std::stop_token, std::string&)> writer, std::size_t total = 0);
    void confirm();
    void cancel();
    void fail(std::string error);
    DataTransferStatus status() const;
    std::optional<DataImportBatch> take(std::size_t maxSamples = 8192, std::size_t maxBytes = 65536);
    void submitted(std::size_t count);

private:
    bool push(DataImportBatch batch, std::stop_token stop);
    void finish(std::string error, bool canceled, bool committed = false);
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    std::deque<DataImportBatch> batches_;
    DataTransferStatus status_;
    bool confirmed_{false};
    std::jthread worker_;
};

}
