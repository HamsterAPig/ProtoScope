#include "protoscope/app/application.hpp"
#include "protoscope/plot/data_file_output.hpp"
#include "protoscope/protocol_utils/codec.hpp"
#include "protoscope/session/session_package.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <yaml-cpp/yaml.h>

namespace protoscope::app {

namespace {

    constexpr std::string_view kSessionProtocolEntryPrefix = "protocol/";

    std::uint64_t rawCaptureEventRxBytes(const std::vector<plot::RawCaptureEvent>& events)
    {
        std::uint64_t total = 0;
        for (const auto& event : events) {
            if (event.type == plot::RawCaptureEventType::RxBytes) {
                total += static_cast<std::uint64_t>(event.bytes.size());
            }
        }
        return total;
    }


    std::vector<std::uint8_t> stringBytes(std::string_view text)
    {
        return {text.begin(), text.end()};
    }


    std::string stringFromBytes(const std::vector<std::uint8_t>& bytes)
    {
        return {bytes.begin(), bytes.end()};
    }


    std::vector<std::uint8_t> readFileBytes(const std::filesystem::path& path, std::string& error)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.good()) {
            error = "无法读取文件: " + path.generic_string();
            return {};
        }
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }


    bool isSafeSessionPackageEntryName(std::string_view name)
    {
        if (name.empty() || name.front() == '/' || name.front() == '\\') {
            return false;
        }
        if (name.size() >= 2 && name[1] == ':' &&
            ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z'))) {
            return false;
        }

        std::size_t segmentStart = 0;
        for (std::size_t index = 0; index <= name.size(); ++index) {
            const bool atEnd = index == name.size();
            const bool separator = !atEnd && (name[index] == '/' || name[index] == '\\');
            if (!atEnd && name[index] == ':') {
                return false;
            }
            if (!atEnd && !separator) {
                continue;
            }

            const auto segment = name.substr(segmentStart, index - segmentStart);
            if (segment.empty() || segment == "." || segment == "..") {
                return false;
            }
            segmentStart = index + 1;
        }
        return true;
    }


    bool validateSessionPackageEntries(const session::SessionPackageData& package, std::string& error)
    {
        for (const auto& entry : package.entries) {
            if (!isSafeSessionPackageEntryName(entry.name)) {
                error = "现场包包含不安全条目: " + entry.name;
                return false;
            }
        }
        return true;
    }


    bool collectProtocolDirectoryEntries(const std::filesystem::path& protocolDir,
                                         std::vector<session::SessionPackageEntry>& entries,
                                         std::string& error)
    {
        std::error_code pathError;
        if (!std::filesystem::is_directory(protocolDir, pathError) || pathError) {
            error = "当前协议目录不可读取: " + protocolDir.generic_string();
            if (pathError) {
                error += ", " + pathError.message();
            }
            return false;
        }

        pathError = {};
        const auto mainLuaPath = protocolDir / "main.lua";
        if (!std::filesystem::is_regular_file(mainLuaPath, pathError) || pathError) {
            error = "当前协议目录缺少 main.lua: " + mainLuaPath.generic_string();
            if (pathError) {
                error += ", " + pathError.message();
            }
            return false;
        }

        std::error_code walkError;
        for (std::filesystem::recursive_directory_iterator
                 it(protocolDir, std::filesystem::directory_options::skip_permission_denied, walkError),
             end;
             it != end;
             it.increment(walkError)) {
            if (walkError) {
                error = "遍历当前协议目录失败: " + walkError.message();
                return false;
            }

            std::error_code typeError;
            if (!it->is_regular_file(typeError)) {
                if (typeError) {
                    error = "检查协议文件类型失败: " + typeError.message();
                    return false;
                }
                continue;
            }

            const auto relativePath = it->path().lexically_relative(protocolDir);
            const auto relativeName = relativePath.generic_string();
            if (relativeName.empty()) {
                error = "协议目录包含无法打包的空相对路径";
                return false;
            }

            std::string readError;
            auto bytes = readFileBytes(it->path(), readError);
            if (!readError.empty()) {
                error = "读取当前协议目录文件失败: " + readError;
                return false;
            }
            // 核心流程：现场包必须保留协议目录结构，导入后 Lua 的 require 同目录模块才能继续生效。
            entries.push_back(session::SessionPackageEntry{
                .name = std::string(kSessionProtocolEntryPrefix) + relativeName,
                .bytes = std::move(bytes),
            });
        }

        std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) { return lhs.name < rhs.name; });
        return true;
    }


    std::string encodeAnalysisMarkersYaml(const std::vector<plot::WaveAnalysisMarker>& markers)
    {
        YAML::Node root;
        for (const auto& marker : markers) {
            YAML::Node item;
            item["id"] = marker.id;
            item["label"] = marker.label;
            item["note"] = marker.note;
            item["start_time"] = marker.startTime;
            item["end_time"] = marker.endTime;
            item["channel_index"] = marker.channelIndex;
            root["markers"].push_back(item);
        }
        return YAML::Dump(root);
    }


    std::vector<plot::WaveAnalysisMarker> decodeAnalysisMarkersYaml(std::string_view yamlText)
    {
        std::vector<plot::WaveAnalysisMarker> markers;
        const auto root = YAML::Load(std::string(yamlText));
        const auto node = root["markers"];
        if (!node || !node.IsSequence()) {
            return markers;
        }
        for (const auto& item : node) {
            plot::WaveAnalysisMarker marker;
            marker.id = item["id"].as<std::uint64_t>(0);
            marker.label = item["label"].as<std::string>("");
            marker.note = item["note"].as<std::string>("");
            marker.startTime = item["start_time"].as<double>(0.0);
            marker.endTime = item["end_time"].as<double>(marker.startTime);
            marker.channelIndex = item["channel_index"].as<std::size_t>(0);
            markers.push_back(std::move(marker));
        }
        return markers;
    }


    void applyActiveRawProfileEvent(std::vector<plot::RawCaptureEvent>& activeProfiles,
                                    const plot::RawCaptureEvent& event)
    {
        if (event.type == plot::RawCaptureEventType::ProfileClear && event.profile.frameName.empty()) {
            activeProfiles.clear();
            return;
        }

        const auto sameFrame = [&event](const plot::RawCaptureEvent& item) {
            return item.profile.frameName == event.profile.frameName;
        };
        activeProfiles.erase(std::remove_if(activeProfiles.begin(), activeProfiles.end(), sameFrame),
                             activeProfiles.end());
        if (event.type == plot::RawCaptureEventType::ProfileSet) {
            activeProfiles.push_back(event);
        }
    }


    std::optional<plot::RawCapturePlotSetupEventData> currentPlotSetupSnapshot(const plot::WaveDockState& wave,
                                                                               std::string source)
    {
        if (wave.defaultChannelSpecs.empty()) {
            return std::nullopt;
        }
        plot::RawCapturePlotSetupEventData setup;
        setup.source = std::move(source);
        setup.channels = wave.defaultChannelSpecs;
        setup.view = wave.buffer.viewConfig();
        setup.resetHistory = false;
        return setup;
    }


    std::vector<plot::RawCaptureEvent> trimRawCaptureEventsToPayloadWindow(
        const std::vector<plot::RawCaptureEvent>& events, std::uint64_t keepStart, bool includeTx = false)
    {
        std::vector<plot::RawCaptureEvent> activeProfiles;
        std::optional<plot::RawCaptureEvent> activePlotSetup;
        std::vector<plot::RawCaptureEvent> keptEvents;
        std::uint64_t rxCursor = 0;

        for (const auto& event : events) {
            if (event.type != plot::RawCaptureEventType::RxBytes &&
                !(includeTx && event.type == plot::RawCaptureEventType::TxBytes)) {
                if (rxCursor < keepStart) {
                    if (event.type == plot::RawCaptureEventType::ProfileSet ||
                        event.type == plot::RawCaptureEventType::ProfileClear) {
                        applyActiveRawProfileEvent(activeProfiles, event);
                    } else if (event.type == plot::RawCaptureEventType::PlotSetup) {
                        activePlotSetup = event;
                    }
                } else {
                    keptEvents.push_back(event);
                }
                continue;
            }

            const auto eventStart = rxCursor;
            const auto eventSize = static_cast<std::uint64_t>(event.bytes.size());
            const auto eventEnd = eventStart + eventSize;
            rxCursor = eventEnd;
            if (eventEnd <= keepStart || event.bytes.empty()) {
                continue;
            }

            plot::RawCaptureEvent kept = event;
            if (eventStart < keepStart) {
                const auto trimPrefix = static_cast<std::size_t>(keepStart - eventStart);
                kept.bytes.assign(event.bytes.begin() + static_cast<std::ptrdiff_t>(trimPrefix), event.bytes.end());
            }
            keptEvents.push_back(std::move(kept));
        }

        std::vector<plot::RawCaptureEvent> activeEvents;
        activeEvents.reserve(activeProfiles.size() + keptEvents.size() + (activePlotSetup.has_value() ? 1U : 0U));
        activeEvents.insert(activeEvents.end(),
                            std::make_move_iterator(activeProfiles.begin()),
                            std::make_move_iterator(activeProfiles.end()));
        if (activePlotSetup.has_value()) {
            activeEvents.push_back(std::move(*activePlotSetup));
        }
        activeEvents.insert(
            activeEvents.end(), std::make_move_iterator(keptEvents.begin()), std::make_move_iterator(keptEvents.end()));
        return activeEvents;
    }


    void normalizeRawCaptureExportWindow(plot::RawCaptureFileData& capture)
    {
        // 核心流程：普通导出只保存当前 raw 窗口，但必须保留回放所依赖的运行时 profile/plot setup 事件。
        const auto eventRxBytes = rawCaptureEventRxBytes(capture.events);
        if (!capture.events.empty() && eventRxBytes >= capture.payload.size()) {
            const auto keepStart = eventRxBytes - static_cast<std::uint64_t>(capture.payload.size());
            capture.events = trimRawCaptureEventsToPayloadWindow(capture.events, keepStart);
            return;
        }

        if (!capture.payload.empty()) {
            capture.events.clear();
            capture.events.push_back(plot::RawCaptureEvent{
                .type = plot::RawCaptureEventType::RxBytes,
                .timestampMs = capture.capturedAtMs,
                .bytes = capture.payload,
                .profile = {},
                .plotSetup = {},
            });
            return;
        }

        capture.events = trimRawCaptureEventsToPayloadWindow(capture.events, eventRxBytes);
    }


    bool prepareRawCaptureForExport(plot::RawCaptureFileData& capture,
                                    const std::string& protocolName,
                                    const std::string& protocolDir,
                                    double sampleFrequencyHz,
                                    std::uint64_t capturedAtMs,
                                    std::string_view metadataError,
                                    std::string& error)
    {
        capture.protocolName = protocolName.empty() ? capture.protocolName : protocolName;
        capture.protocolDir = protocolDir.empty() ? capture.protocolDir : protocolDir;
        capture.sampleFrequencyHz = sampleFrequencyHz;
        if (capture.capturedAtMs == 0) {
            capture.capturedAtMs = capturedAtMs;
        }

        if (capture.protocolName.empty() || capture.protocolDir.empty()) {
            // 原始记录和波形快照可独立于协议再次导出，协议只在显式离线解析时必需。
            static_cast<void>(metadataError);
            static_cast<void>(error);
        }

        normalizeRawCaptureExportWindow(capture);
        return true;
    }


    struct SessionLogSummaryCounts {
        std::size_t transferRows{0};
        std::size_t transferFrameRows{0};
        std::size_t hostLogRows{0};
        std::size_t scriptLogRows{0};
        std::size_t requestTraceRows{0};
        std::size_t rawCaptureEvents{0};
        std::size_t rawCaptureBytes{0};
    };


    std::string buildSessionLogSummary(const SessionLogSummaryCounts& counts)
    {
        std::ostringstream logSummary;
        logSummary << "transfer_rows: " << counts.transferRows << '\n';
        logSummary << "transfer_frame_rows: " << counts.transferFrameRows << '\n';
        logSummary << "host_log_rows: " << counts.hostLogRows << '\n';
        logSummary << "script_log_rows: " << counts.scriptLogRows << '\n';
        logSummary << "request_trace_rows: " << counts.requestTraceRows << '\n';
        logSummary << "raw_capture_events: " << counts.rawCaptureEvents << '\n';
        logSummary << "raw_capture_bytes: " << counts.rawCaptureBytes << '\n';
        return logSummary.str();
    }


    std::vector<dock::ReceiveRow> copyReceiveRows(const std::deque<dock::ReceiveRow>& rows)
    {
        return std::vector<dock::ReceiveRow>(rows.begin(), rows.end());
    }


    std::vector<dock::RequestTraceRow> copyRequestTraceRows(const std::deque<dock::RequestTraceRow>& rows)
    {
        return std::vector<dock::RequestTraceRow>(rows.begin(), rows.end());
    }


    std::string buildSessionManifest(std::uint64_t createdAtMs,
                                     const std::string& protocolName,
                                     const std::string& protocolDir,
                                     double sampleFrequencyHz,
                                     const std::vector<session::SessionPackageEntry>& entries)
    {
        std::ostringstream manifest;
        manifest << "format: protoscope_session_package\n";
        manifest << "version: 1\n";
        manifest << "created_at_ms: " << createdAtMs << '\n';
        manifest << "protocol_name: " << protocolName << '\n';
        manifest << "protocol_dir: " << protocolDir << '\n';
        manifest << "sample_frequency_hz: " << sampleFrequencyHz << '\n';
        manifest << "entries:\n";
        manifest << "- manifest.yaml\n";
        for (const auto& entry : entries) {
            manifest << "- " << entry.name << '\n';
        }
        return manifest.str();
    }


    session::SessionPackageData buildSessionPackage(std::uint64_t createdAtMs,
                                                    std::string configYaml,
                                                    std::vector<std::uint8_t> rawCaptureBytes,
                                                    std::vector<session::SessionPackageEntry> protocolEntries,
                                                    std::string analysisMarkersYaml,
                                                    std::string logSummary,
                                                    std::string hostLogText,
                                                    std::string scriptLogText,
                                                    std::string requestTraceText,
                                                    const std::string& manifestProtocolName,
                                                    const std::string& manifestProtocolDir,
                                                    double sampleFrequencyHz)
    {
        std::vector<session::SessionPackageEntry> entries;
        entries.push_back({.name = "config.yaml", .bytes = stringBytes(configYaml)});
        entries.push_back({.name = "raw_capture.psraw", .bytes = std::move(rawCaptureBytes)});
        entries.insert(entries.end(),
                       std::make_move_iterator(protocolEntries.begin()),
                       std::make_move_iterator(protocolEntries.end()));
        entries.push_back({.name = "analysis/markers.yaml", .bytes = stringBytes(analysisMarkersYaml)});
        entries.push_back({.name = "logs/summary.txt", .bytes = stringBytes(logSummary)});
        if (!hostLogText.empty()) {
            entries.push_back({.name = "logs/host.txt", .bytes = stringBytes(hostLogText)});
        }
        if (!scriptLogText.empty()) {
            entries.push_back({.name = "logs/script.txt", .bytes = stringBytes(scriptLogText)});
        }
        if (!requestTraceText.empty()) {
            entries.push_back({.name = "logs/request_trace.csv", .bytes = stringBytes(requestTraceText)});
        }

        const auto manifest =
            buildSessionManifest(createdAtMs, manifestProtocolName, manifestProtocolDir, sampleFrequencyHz, entries);
        entries.insert(entries.begin(), {.name = "manifest.yaml", .bytes = stringBytes(manifest)});

        return session::SessionPackageData{
            .createdAtMs = createdAtMs,
            .entries = std::move(entries),
        };
    }


    bool decodeSessionRawCaptureEntry(const session::SessionPackageData& package,
                                      const std::optional<std::string>& importedProtocolDir,
                                      std::optional<plot::RawCaptureFileData>& rawCapture,
                                      std::string& error)
    {
        const auto* rawEntry = session::findSessionPackageEntry(package, "raw_capture.psraw");
        if (rawEntry == nullptr || rawEntry->bytes.empty()) {
            return true;
        }

        const auto rawText =
            std::string_view(reinterpret_cast<const char*>(rawEntry->bytes.data()), rawEntry->bytes.size());
        auto capture = plot::decodeRawCaptureFile(rawText, error);
        if (!capture.has_value()) {
            return false;
        }
        if (importedProtocolDir.has_value()) {
            capture->protocolDir = *importedProtocolDir;
        }
        rawCapture = std::move(*capture);
        return true;
    }


    bool decodeSessionAnalysisMarkers(const session::SessionPackageData& package,
                                      std::vector<plot::WaveAnalysisMarker>& markers,
                                      std::string& error)
    {
        const auto* markerEntry = session::findSessionPackageEntry(package, "analysis/markers.yaml");
        if (markerEntry == nullptr) {
            return true;
        }

        try {
            const auto markerText =
                std::string_view(reinterpret_cast<const char*>(markerEntry->bytes.data()), markerEntry->bytes.size());
            markers = decodeAnalysisMarkersYaml(markerText);
            return true;
        } catch (const std::exception& ex) {
            error = std::string("解析现场包分析标记失败: ") + ex.what();
            return false;
        }
    }


    plot::RawCaptureEvent makeRawCapturePlotSetupEvent(std::uint64_t timestampMs,
                                                       plot::RawCapturePlotSetupEventData setup)
    {
        return plot::RawCaptureEvent{
            .type = plot::RawCaptureEventType::PlotSetup,
            .timestampMs = timestampMs,
            .bytes = {},
            .profile = {},
            .plotSetup = std::move(setup),
        };
    }

} // namespace

std::string Application::csvEscape(std::string_view text)
{
    std::string escaped;
    escaped.reserve(text.size() + 2);
    escaped.push_back('"');
    for (const char ch : text) {
        if (ch == '"') {
            escaped.push_back('"');
        }
        escaped.push_back(ch);
    }
    escaped.push_back('"');
    return escaped;
}

void Application::appendLiveRawCapture(const transport::TransportBytesEvent& event)
{
    auto& wave = dockStore_.waveState();
    const auto& lua = dockStore_.luaState();
    if (!wave.rawCapture.payload.empty() &&
        (wave.rawCapture.protocolDir != lua.protocolDir || wave.rawCapture.protocolName != lua.protocolName)) {
        wave.rawCapture = {};
    }
    if (wave.rawCapture.events.empty()) {
        wave.rawCapture.capturedAtMs = event.context.timestampMs;
    }
    wave.rawCapture.protocolName = lua.protocolName;
    wave.rawCapture.protocolDir = lua.protocolDir;
    wave.rawCapture.sampleFrequencyHz = wave.view.sampleFrequencyHz;
    appendRawCaptureEvent(plot::RawCaptureEvent{
        .type = plot::RawCaptureEventType::RxBytes,
        .timestampMs = event.context.timestampMs,
        .bytes = event.bytes,
        .profile = {},
        .plotSetup = {},
        .endpoint = event.context.endpoint,
    });

    const auto limit = runtimeConfig_.gui.rawCapture.liveLimitBytes;
    if (wave.rawCapture.payload.size() <= limit) {
        return;
    }

    // 核心流程：实时接收只保存最近一段原始字节，完整历史应由显式录制或外部文件承载。
    wave.rawCapture.truncated = true;
    const auto keepStart = rawCaptureEventRxBytes(wave.rawCapture.events) > limit
                               ? rawCaptureEventRxBytes(wave.rawCapture.events) - limit
                               : 0U;
    wave.rawCapture.events = trimRawCaptureEventsToPayloadWindow(wave.rawCapture.events, keepStart);
    if (limit == 0U) {
        wave.rawCapture.payload.clear();
        return;
    }
    const auto removeCount = wave.rawCapture.payload.size() - limit;
    wave.rawCapture.payload.erase(
        wave.rawCapture.payload.begin(),
        wave.rawCapture.payload.begin() + static_cast<std::vector<std::uint8_t>::difference_type>(removeCount));
}


void Application::appendRawCaptureEvent(const plot::RawCaptureEvent& event)
{
    if (replayReceiveHistory_ && !activeConnection_) return;
    auto& rawCapture = dockStore_.waveState().rawCapture;
    if (rawCapture.protocolName.empty()) rawCapture.protocolName = dockStore_.luaState().protocolName;
    if (rawCapture.protocolDir.empty()) rawCapture.protocolDir = dockStore_.luaState().protocolDir;
    if (rawCapture.events.empty()) retainedRawBytes_ = 0;
    rawCapture.events.push_back(event);
    rawCapture.events.back().sequence = ++rawEventSequence_;
    rawCapture.events.back().displayPosition = ++transferPosition_;
    rawCapture.source = "live";
    if (event.type == plot::RawCaptureEventType::TxBytes) rawCapture.rxOnly = false;
    if (rawCapture.capturedAtMs == 0) rawCapture.capturedAtMs = event.timestampMs;
    if (rawCaptureRecording_.isOpen()) {
        std::string error;
        if (!rawCaptureRecording_.appendEvent(rawCapture.events.back(), error)) {
            std::string ignored;
            rawCaptureRecording_.close(ignored);
            setStatusMessage("原始录制失败: " + error);
        }
    }
    if (event.type == plot::RawCaptureEventType::RxBytes) {
        rawCapture.payload.insert(rawCapture.payload.end(), event.bytes.begin(), event.bytes.end());
    }
    retainedRawBytes_ += event.bytes.size();
    const auto limit = runtimeConfig_.gui.rawCapture.liveLimitBytes;
    // RX/TX 共用保留预算，配置事件仍保留；文件导入不经过此实时裁剪入口。
    if (retainedRawBytes_ > limit) {
        if (historyRebuild_ && !historyRebuild_->rowsOnly) {
            invalidateHistorySwitchWork("底层历史达到裁剪预算");
            dockStore_.waveState().statusMessage = "历史重建已取消：底层历史达到裁剪预算";
        }
        rawCapture.events = trimRawCaptureEventsToPayloadWindow(rawCapture.events, retainedRawBytes_ - limit, true);
        retainedRawBytes_ = limit;
        rawCapture.truncated = true;
        rawCapture.payload.clear();
        for (const auto& retained : rawCapture.events)
            if (retained.type == plot::RawCaptureEventType::RxBytes)
                rawCapture.payload.insert(rawCapture.payload.end(), retained.bytes.begin(), retained.bytes.end());
    }
}


void Application::recordPlotSetupSnapshot(const plot::RawCapturePlotSetupEventData& setup, std::uint64_t timestampMs)
{
    const plot::RawCaptureEvent event{
        .type = plot::RawCaptureEventType::PlotSetup,
        .timestampMs = timestampMs,
        .bytes = {},
        .profile = {},
        .plotSetup = setup,
    };
    if (!suppressRawCapturePlotSetupEvents_) {
        appendRawCaptureEvent(event);
    }
}


bool Application::exportWaveRawCapture(const std::filesystem::path& path, std::string& error) const
{
    return exportWaveRawCapture(path, plot::CsvExportRange{}, error);
}


bool Application::exportWaveRawCapture(const std::filesystem::path& path,
                                       const plot::CsvExportRange& range,
                                       std::string& error) const
{
    const auto& lua = dockStore_.luaState();
    const auto& wave = dockStore_.waveState();
    plot::RawCaptureFileData capture = wave.rawCapture;
    if (!prepareRawCaptureForExport(capture,
                                    lua.protocolName,
                                    lua.protocolDir,
                                    wave.view.sampleFrequencyHz,
                                    nowMs(),
                                    "当前协议元数据不完整，无法导出",
                                    error)) {
        return false;
    }
    if (const auto timeRange = plot::resolveCsvExportTimeRange(range); timeRange.has_value()) {
        std::vector<plot::RawCaptureEvent> filteredEvents;
        filteredEvents.reserve(capture.events.size());
        capture.payload.clear();
        for (const auto& event : capture.events) {
            const double elapsedSeconds =
                (static_cast<double>(event.timestampMs) - static_cast<double>(capture.capturedAtMs)) / 1000.0;
            if (elapsedSeconds < timeRange->first || elapsedSeconds > timeRange->second) {
                continue;
            }
            filteredEvents.push_back(event);
            if (event.type == plot::RawCaptureEventType::RxBytes) {
                capture.payload.insert(capture.payload.end(), event.bytes.begin(), event.bytes.end());
            }
        }
        capture.events = std::move(filteredEvents);
    }
    return plot::writeRawCaptureFile(path, capture, error);
}


bool Application::exportWaveCsv(const std::filesystem::path& path,
                                plot::WaveCsvShape shape,
                                const plot::CsvExportRange& range,
                                std::string& error) const
{
    const auto data = captureWaveData(range, error);
    return data && plot::writeWaveCsvFile(path, *data, shape, {}, error);
}


void Application::confirmDataImport(const OfflineReplayProcessing processing, const OfflineReplayPacing pacing)
{
    // 持久来源偏好对初始化与可选历史解析一致生效，使用当前时绝不执行包内 Lua。
    dataImportProcessing_ = processing == OfflineReplayProcessing::PackageProtocol &&
        (runtimeConfig_.gui.importProtocolSource == config::ImportProtocolSource::CurrentProtocol ||
         !dataTransfer_.status().hasPackageProtocol) ? OfflineReplayProcessing::CurrentProtocol : processing;
    dataImportPacing_ = pacing;
    parseImportedWave_ = dataImportProcessing_ != OfflineReplayProcessing::BrowseRaw;
    dataTransfer_.confirm();
}


void Application::resetDataImportGenerationState(const bool cleanupReplayResources)
{
    clearTransferHistoryStatus("导入来源重置");
    importReset_.reset();
    importProfile_.reset();
    if (importProtocolLoad_.valid()) {
        // 取消无法中断已执行初始化；串行排入旧协议恢复，期间不发布过期输出。
        const auto previousProtocol = dockStore_.luaState().protocolDir;
        retiringImportProtocolLoad_ = previousProtocol.empty() ? importProtocolLoad_
            : scriptWorker_.loadProtocolDirectoryAsync(previousProtocol).share();
        if (pendingImportPreviousConfig_) {
            configureScriptWorker(*pendingImportPreviousConfig_);
        }
    }
    importProtocolLoad_ = {};
    pendingImportMetadata_.reset();
    importProtocolPrepared_ = false;
    pendingImportPreviousConfig_.reset();
    pendingImportProtocol_.clear();
    pendingImportProtocolDirectory_.clear();
    pendingImportSession_.reset();
    pendingImportMarkers_.clear();
    pendingImportedCapture_ = {};
    suppressRawCaptureProfileEvents_ = false;
    suppressRawCapturePlotSetupEvents_ = false;
    pendingScriptPlotAppends_.clear();
    pendingTransferFrameRows_.clear();
    if (cleanupReplayResources) cleanupOfflineReplayResources();
}


void Application::cancelDataTransfer()
{
    dataTransfer_.cancel();
    ++rawCaptureReplay_.generation;
    rawCaptureReplay_.phase = OfflineReplayPhase::Cancelled;
    rawCaptureReplay_.playing = false;
    resetDataImportGenerationState(true);
}


bool Application::startDataImport(const std::filesystem::path& path, std::string& error)
{
    if (dataTransferStatus().active) { error = "已有数据任务正在执行"; return false; }
    if (retiringImportProtocolLoad_.valid()) {
        static_cast<void>(scriptWorker_.drainOutputs());
        if (retiringImportProtocolLoad_.wait_for(std::chrono::seconds(0)) != std::future_status::ready ||
            !scriptWorker_.inputIdle()) {
            error = "已取消的协议准备尚未结束，请稍后重新导入";
            return false;
        }
        const auto result = retiringImportProtocolLoad_.get();
        retiringImportProtocolLoad_ = {};
        static_cast<void>(scriptWorker_.drainOutputs());
        // 被取消的准备不得提交旧代次路径、波形或记录；下一次导入会显式加载其所选协议。
        if (!result.ok) loggingFacade_.warn("session", result.lastError);
    }
    if (!validateOfflineReplayTransport(error)) return false;
    if (rawCaptureRecording_.isOpen()) { error = "请先停止原始录制"; return false; }
    if (rawCaptureReplay_.loaded) unloadRawCaptureReplayTimeline();
    cancelAllTxRequests("开始离线导入");
    importReplacedWave_ = false;
    parseImportedWave_ = false;
    importReplacedRecords_ = false;
    resetDataImportGenerationState(true);
    dataImportProcessing_ = OfflineReplayProcessing::BrowseRaw;
    dataImportPacing_ = OfflineReplayPacing::FastBatchParse;
    dataImportActive_ = dataTransfer_.startImport(path);
    return dataImportActive_;
}


bool Application::pumpDataImport()
{
    const auto status = dataTransfer_.status();
    if (rawCaptureReplay_.loaded && !status.awaitingConfirmation && !status.active) {
        std::string replayError;
        pumpRawCaptureReplay(replayError);
        if (rawCaptureReplay_.phase == OfflineReplayPhase::Preparing ||
            rawCaptureReplay_.phase == OfflineReplayPhase::Replaying ||
            rawCaptureReplay_.phase == OfflineReplayPhase::Draining) {
            return true;
        }
        if (rawCaptureReplay_.phase == OfflineReplayPhase::Failed) {
            dataTransfer_.fail(replayError.empty() ? rawCaptureReplay_.error : replayError);
        }
    }
    const auto started = std::chrono::steady_clock::now();
    std::size_t samples = 0;
    std::size_t bytes = 0;
    try {
        if (importProtocolLoad_.valid()) {
            // 加载期间只消费旧 worker 输出以解除背压，不把初始化波形写入旧快照。
            static_cast<void>(scriptWorker_.drainOutputs());
            if (importProtocolLoad_.wait_for(std::chrono::seconds(0)) != std::future_status::ready ||
                !scriptWorker_.inputIdle()) return true;
            const auto result = importProtocolLoad_.get();
            importProtocolLoad_ = {};
            if (!result.ok) {
                if (pendingImportPreviousConfig_) {
                    configureScriptWorker(*pendingImportPreviousConfig_);
                }
                throw std::runtime_error("所选导入协议加载失败: " + result.lastError);
            }
            // 加载成功才更新协议面板；不走 reload 的历史刷新/波形初始化输出提交。
            auto& lua = dockStore_.luaState();
            lua.protocolDir = pendingImportProtocol_;
            lua.protocolName = configStore_.protocolName(pendingImportProtocol_);
            lua.scriptPath = configStore_.mainLuaPath(pendingImportProtocol_).generic_string();
            lua.loaded = true;
            lua.lastError.clear();
            applyLuaScriptSnapshot(result.snapshot);
            static_cast<void>(scriptWorker_.drainOutputs());
            resetTransferFrameParser();
            importProtocolPrepared_ = true;
        }
        // 协议加载和状态重置都由脚本线程执行；UI 只轮询，不能 runSync 等待。
        if (rawCaptureReplay_.phase == OfflineReplayPhase::Preparing && rawCaptureReplay_.preparation.valid()) {
            if (rawCaptureReplay_.preparation.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return true;
            const auto result = rawCaptureReplay_.preparation.get();
            rawCaptureReplay_.preparation = {};
            if (!result.ok) throw std::runtime_error("离线协议准备失败: " + result.lastError);
            prepareRawCaptureImportReplay(rawCaptureReplay_.capture);
            rawCaptureReplay_.phase = OfflineReplayPhase::Replaying;
        }
        if (importReset_) {
            if (importReset_->wait_for(std::chrono::seconds(0)) != std::future_status::ready) return true;
            if (!importReset_->get()) throw std::runtime_error("重置离线解析状态失败");
            importReset_.reset();
        }
        if (importProfile_) {
            if (importProfile_->wait_for(std::chrono::seconds(0)) != std::future_status::ready) return true;
            const auto result = importProfile_->get();
            importProfile_.reset();
            if (!result.first) throw std::runtime_error(result.second);
        }
        if (parseImportedWave_) {
            flushOfflineScriptOutputs();
            if (offlineScriptWorker_ && offlineScriptWorker_->pendingRxBytes() > 65536) return true;
        }
        while (samples < 8192 && bytes < 65536 &&
               std::chrono::steady_clock::now() - started < std::chrono::milliseconds(4)) {
            if (parseImportedWave_ &&
                (importReset_ || importProfile_ || (offlineScriptWorker_ && !offlineScriptWorker_->inputIdle()))) break;
            auto batch = pendingImportMetadata_ ? std::move(pendingImportMetadata_)
                                                : dataTransfer_.take(8192 - samples, 65536 - bytes);
            pendingImportMetadata_.reset();
            if (!batch) break;
            if (batch->id != status.id) continue;
            samples += batch->samples.size();
            if (batch->event) bytes += (std::max<std::size_t>)(1, batch->event->bytes.size());
            for (const auto& event : batch->events) bytes += (std::max<std::size_t>)(1, event.bytes.size());
            if (batch->event) batch->events.push_back(std::move(*batch->event));
            auto& wave = dockStore_.waveState();
            auto& receive = dockStore_.receiveState();
            if (batch->metadata) {
                std::string error;
                if (!validateImportedFrequency(*batch->metadata, error)) throw std::runtime_error(error);
                pendingImportSession_ = batch->session;
                if (batch->session && !importProtocolPrepared_) {
                    pendingImportPreviousConfig_ = runtimeConfig_;
                    pendingImportPreviousConfig_->protocol.selectedDir = dockStore_.luaState().protocolDir;
                    if (!validateSessionPackageEntries(*batch->session, error) ||
                        !selectSessionImportProtocol(*batch->session, pendingImportProtocol_,
                                                     pendingImportProtocolDirectory_, error))
                        throw std::runtime_error(error);
                    if (!pendingImportProtocolDirectory_.empty())
                        importedProtocolDirectories_.push_back(pendingImportProtocolDirectory_);
                    if (!prepareSessionScriptConfig(*batch->session, error))
                        throw std::runtime_error(error);
                    if (!pendingImportProtocol_.empty()) {
                        importProtocolLoad_ = scriptWorker_.loadProtocolDirectoryAsync(pendingImportProtocol_).share();
                        pendingImportMetadata_ = std::move(batch);
                        return true;
                    }
                    // 无当前协议且无包协议时允许浏览快照/原始记录，不制造无效加载任务。
                }
                if (batch->session && !restoreSessionPackageContext(*batch->session,
                        batch->metadata->waveform.has_value(), pendingImportMarkers_, error))
                    throw std::runtime_error(error);
                if (dataImportProcessing_ == OfflineReplayProcessing::PackageProtocol && !batch->session) {
                    throw std::runtime_error("当前导入文件不包含现场包协议");
                }
                if (batch->metadata->waveform) {
                    parseImportedWave_ = false;
                    if (!importWaveCsvData(*batch->metadata->waveform, error)) throw std::runtime_error(error);
                    importReplacedWave_ = true;
                    importedWaveIncomplete_ = true;
                }
                applyImportedFrequency(*batch->metadata);
                if (status.includesRecords) {
                    pendingImportedCapture_ = *batch->metadata;
                    pendingImportedCapture_.events.clear();
                    pendingImportedCapture_.payload.clear();
                    importReplacedRecords_ = true;
                    if (batch->metadata->waveform) wave.rawCapture.incomplete = true;
                }
            } else if (!batch->samples.empty()) {
                wave.buffer.appendImported(batch->channel, batch->samples, batch->sampleIndexOffset);
                dataTransfer_.submitted(batch->samples.size());
            } else if (!pendingImportedCapture_.protocolDir.empty() || status.includesRecords) {
                for (auto& event : batch->events) {
                    pendingImportedCapture_.events.push_back(event);
                    if (event.type == plot::RawCaptureEventType::RxBytes) {
                        pendingImportedCapture_.payload.insert(
                            pendingImportedCapture_.payload.end(), event.bytes.begin(), event.bytes.end());
                    } else if (event.type == plot::RawCaptureEventType::TxBytes) {
                        pendingImportedCapture_.rxOnly = false;
                    }
                    dataTransfer_.submitted(1);
                }
            } else for (auto& event : batch->events) {
                auto& capture = wave.rawCapture;
                event.displayPosition = ++transferPosition_;
                rememberTransferProfile(event);
                const bool bytesEvent = event.type == plot::RawCaptureEventType::RxBytes ||
                                        event.type == plot::RawCaptureEventType::TxBytes;
                if (event.type == plot::RawCaptureEventType::TxBytes) capture.rxOnly = false;
                if (parseImportedWave_) {
                    auto context = makeRawCaptureReplayContext(capture);
                    context.timestampMs = event.timestampMs;
                    context.endpoint = event.endpoint;
                    std::string replayError;
                    if (!replayRawCaptureEvent(event, context, replayError)) throw std::runtime_error(replayError);
                }
                if (event.type == plot::RawCaptureEventType::RxBytes)
                    capture.payload.insert(capture.payload.end(), event.bytes.begin(), event.bytes.end());
                capture.events.push_back(event);
                if (bytesEvent) {
                    // 大事件必须先完整组装，再同时提交 Lua 与原始记录，保持一个现代事件一个 callback。
                    receive.rows.push_back({.timestampMs = event.timestampMs,
                        .direction = event.type == plot::RawCaptureEventType::TxBytes ? "TX" : "RX",
                        .endpoint = event.endpoint, .bytes = event.bytes, .message = event.writeStatus,
                        .messageIsWriteStatus = event.type == plot::RawCaptureEventType::TxBytes,
                        .displayPosition = event.displayPosition, .historyProfiles = transferProfiles_});
                }
                dataTransfer_.submitted(1);
                ++receive.rowsVersion;
            }
        }
    } catch (const std::exception& ex) {
        applyingImportContext_ = false;
        resetDataImportGenerationState(true);
        dataTransfer_.fail(std::string(importReplacedWave_ || importReplacedRecords_
            ? "导入提交失败，已保留部分数据: " : "导入准备失败，已保留原有数据: ") + ex.what());
    }
    const auto current = dataTransfer_.status();
    if (!current.active && current.complete) {
        if (!rawCaptureReplay_.loaded && !pendingImportedCapture_.events.empty()) {
            std::string replayError;
            if (dataImportProcessing_ == OfflineReplayProcessing::BrowseRaw) {
                if (!importRawRecords(pendingImportedCapture_, replayError)) {
                    dataTransfer_.fail(replayError);
                    return true;
                }
            } else {
                if (!loadRawCaptureReplayTimeline(pendingImportedCapture_,
                                                  dataImportProcessing_,
                                                  dataImportPacing_,
                                                  replayError,
                                                  pendingImportSession_.get())) {
                    dataTransfer_.fail(replayError);
                    return true;
                }
                rawCaptureReplay_.generation = current.id;
                rawCaptureReplay_.playing = true;
                importReplacedWave_ = true;
                importedWaveIncomplete_ = true;
                return true;
            }
        }
        if (rawCaptureReplay_.loaded &&
            (rawCaptureReplay_.phase == OfflineReplayPhase::Preparing ||
             rawCaptureReplay_.phase == OfflineReplayPhase::Replaying ||
             rawCaptureReplay_.phase == OfflineReplayPhase::Draining)) {
            return true;
        }
            if (parseImportedWave_) {
                flushOfflineScriptOutputs();
                const auto replaySnapshot = offlineScriptWorker_ ? offlineScriptWorker_->snapshot()
                                                                 : scripting::ScriptRuntimeSnapshot{};
                if (importReset_ || importProfile_ || replaySnapshot.pendingWorkerRxBytes > 0U ||
                    replaySnapshot.inputQueueSize > 0U || replaySnapshot.outputQueueSize > 0U ||
                    !pendingScriptPlotAppends_.empty() || !pendingTransferFrameRows_.empty()) return true;
                freezeImportedChannelLabels();

            suppressRawCaptureProfileEvents_ = false;
            suppressRawCapturePlotSetupEvents_ = false;
        }
        if (!pendingImportMarkers_.empty() || pendingImportSession_) {
            dockStore_.waveState().analysisMarkers = pendingImportMarkers_;
        }
        dataImportActive_ = false;
        resetDataImportGenerationState(current.canceled || !current.error.empty());
        const bool incomplete = current.canceled || !current.error.empty();
        if (importReplacedWave_)
            importedWaveIncomplete_ = incomplete || (current.metadata.waveform && current.metadata.waveform->incomplete);
        if (importReplacedRecords_)
            dockStore_.waveState().rawCapture.incomplete = incomplete || current.metadata.incomplete;
        setStatusMessage(!current.error.empty() ? "导入失败: " + current.error :
            current.canceled ? "导入已取消，已提交数据可再次导出" : "数据导入完成");
        syncDockState();
    }
    return true;
}


bool Application::startDataExport(const std::filesystem::path& path, int content, int format,
                                  const plot::CsvExportRange& waveRange, int recordRange,
                                  std::uint64_t recordBeginMs, std::uint64_t recordEndMs,
                                  plot::WaveCsvShape shape, std::string& error)
{
    if (dataTransferStatus().active) { error = "已有数据任务正在执行"; return false; }
    if (content < 0 || content > 3 || format < 0 || format > 3 ||
        (content == 0 && format > 1) || (content == 1 && format > 2) ||
        (content == 2 && format != 0 && format != 2) || (content == 3 && format != 3)) {
        error = "所选内容不支持该格式";
        return false;
    }
    if (content != 0 && (recordRange < 0 || recordRange > 2 ||
                        (recordRange == 2 && recordEndMs < recordBeginMs))) {
        error = "收发时间范围无效"; return false;
    }
    plot::RawCaptureFileData capture;
    if (content == 0 || content == 3) {
        capture.waveform = captureWaveData(waveRange, error);
        if (!capture.waveform) return false;
    }
    std::vector<dock::ReceiveRow> textRows;
    if (content == 1 || content == 3) {
        auto waveform = std::move(capture.waveform);
        capture = dockStore_.waveState().rawCapture;
        capture.waveform = std::move(waveform);
        capture.events.clear();
        capture.payload.clear();
        auto events = dockStore_.waveState().rawCapture.events;
        if (events.empty() && !dockStore_.waveState().rawCapture.payload.empty())
            events.push_back({.timestampMs = capture.capturedAtMs, .bytes = dockStore_.waveState().rawCapture.payload});
        std::size_t byteEvents = 0;
        for (const auto& event : events) {
            const bool bytesEvent = event.type == plot::RawCaptureEventType::RxBytes ||
                                    event.type == plot::RawCaptureEventType::TxBytes;
            dock::ReceiveRow row{.timestampMs = event.timestampMs,
                .direction = event.type == plot::RawCaptureEventType::TxBytes ? "TX" : "RX",
                .endpoint = event.endpoint, .bytes = event.bytes, .message = event.writeStatus,
                        .messageIsWriteStatus = event.type == plot::RawCaptureEventType::TxBytes};
            if (bytesEvent && recordRange == 1 &&
                !dock::matchesLogFilter(row, dockStore_.receiveState().filter, true)) continue;
            if (recordRange == 2 && ((bytesEvent && event.timestampMs < recordBeginMs) ||
                                    event.timestampMs > recordEndMs)) continue;
            capture.events.push_back(event);
            if (event.type == plot::RawCaptureEventType::RxBytes)
                capture.payload.insert(capture.payload.end(), event.bytes.begin(), event.bytes.end());
            if (bytesEvent) { ++byteEvents; textRows.push_back(std::move(row)); }
        }
        if (byteEvents == 0) { error = "所选收发范围没有记录"; return false; }
        capture.filtered = capture.filtered || recordRange != 0;
        if (recordRange == 1) capture.rangeDescription = "filter";
        else if (recordRange == 2)
            capture.rangeDescription = "timestamp_ms:" + std::to_string(recordBeginMs) + ',' + std::to_string(recordEndMs);
    }
    if (content == 2) {
        for (const auto& row : dockStore_.receiveState().frameRows) {
            if (recordRange == 1 && !dock::matchesLogFilter(row, dockStore_.receiveState().filter, true)) continue;
            if (recordRange == 2 && (row.timestampMs < recordBeginMs || row.timestampMs > recordEndMs)) continue;
            textRows.push_back(row);
        }
        if (textRows.empty()) { error = "没有逐帧分析结果"; return false; }
    }
    // 导出发起时冻结唯一现有采样率，不能复用采集时的旧元数据。
    capture.sampleFrequencyHz = dockStore_.waveState().view.sampleFrequencyHz;
    session::SessionPackageData package;
    if (content == 3) {
        std::string configYaml;
        if (!configStore_.saveText(captureConfig(), configYaml, error)) return false;
        std::vector<session::SessionPackageEntry> protocolEntries;
        const auto& lua = dockStore_.luaState();
        // 快照不依赖协议重解析；缺失的协议不应阻止离线现场再次导出。
        if (!lua.protocolDir.empty() && std::filesystem::is_directory(lua.protocolDir) &&
            !collectProtocolDirectoryEntries(lua.protocolDir, protocolEntries, error)) return false;
        package = buildSessionPackage(nowMs(), std::move(configYaml), {}, std::move(protocolEntries),
            encodeAnalysisMarkersYaml(dockStore_.waveState().analysisMarkers), {},
            dock::formatReceiveRowsText(copyReceiveRows(dockStore_.logState().rows), true, true),
            dock::formatReceiveRowsText(copyReceiveRows(dockStore_.scriptState().rows), true, true),
            dock::formatRequestTraceRowsCsv(copyRequestTraceRows(dockStore_.requestTraceState().rows), true),
            lua.protocolName, lua.protocolDir, capture.sampleFrequencyHz);
    }
    std::size_t total = content == 2 ? textRows.size() : capture.events.size();
    if (capture.waveform) for (const auto& channel : capture.waveform->channels) total += channel.samples.size();
    return dataTransfer_.startExport([path, content, format, shape, capture = std::move(capture),
                                      rows = std::move(textRows), package = std::move(package)]
                                     (std::stop_token stop, std::string& taskError) mutable {
        if (stop.stop_requested()) return false;
        if (format == 1) return plot::writeRawCaptureFile(path, capture, taskError);
        if (format == 0 && content == 0)
            return plot::writeWaveCsvFile(path, *capture.waveform, shape, {}, taskError);
        if (format == 0 && content == 1) return plot::writeRawCaptureCsvFile(path, capture, {}, taskError);
        if (format == 3) {
            std::vector<std::uint8_t> bytes;
            if (!plot::encodeRawCaptureFile(capture, bytes, taskError)) return false;
            for (auto& entry : package.entries) if (entry.name == "raw_capture.psraw") entry.bytes = std::move(bytes);
            if (stop.stop_requested()) return false;
            return session::writeSessionPackage(path, package, taskError);
        }
        plot::DataFileOutput output(path);
        output.stream << (content == 2 ? "# kind=analysis_output\n" : "# kind=readable_log\n");
        if (format == 0) {
            output.stream << "# protoscope_csv_version=2\n";
            output.stream << "timestamp_ms,direction,endpoint,bytes_hex,analysis\n";
            for (const auto& row : rows) {
                if (stop.stop_requested()) return false;
                output.stream << row.timestampMs << ',' << csvEscape(row.direction) << ','
                              << csvEscape(row.endpoint) << ',' << protocol_utils::bytesToHex(row.bytes, false)
                              << ',' << csvEscape(row.message) << '\n';
                plot::reportDataFileProgress(1);
            }
        } else {
            for (const auto& row : rows) {
                if (stop.stop_requested()) return false;
                output.stream << dock::formatReceiveRowsText(std::span<const dock::ReceiveRow>(&row, 1), true, true);
                plot::reportDataFileProgress(1);
            }
        }
        return !stop.stop_requested() && output.commit(taskError);
    }, total);
}


bool Application::exportRawCaptureCsv(const std::filesystem::path& path,
                                      const plot::CsvExportRange& range,
                                      std::string& error) const
{
    const auto& lua = dockStore_.luaState();
    const auto& wave = dockStore_.waveState();
    plot::RawCaptureFileData capture = wave.rawCapture;
    if (!prepareRawCaptureForExport(capture,
                                    lua.protocolName,
                                    lua.protocolDir,
                                    wave.view.sampleFrequencyHz,
                                    nowMs(),
                                    "当前协议元数据不完整，无法导出原始事件 CSV",
                                    error)) {
        return false;
    }
    return plot::writeRawCaptureCsvFile(path, capture, range, error);
}


bool Application::exportSessionPackage(const std::filesystem::path& path, std::string& error) const
{
    const auto createdAtMs = nowMs();

    std::string configYaml;
    if (!configStore_.saveText(captureConfig(), configYaml, error)) {
        return false;
    }

    const auto& lua = dockStore_.luaState();
    const auto& wave = dockStore_.waveState();
    plot::RawCaptureFileData capture = wave.rawCapture;
    if (!prepareRawCaptureForExport(capture,
                                    lua.protocolName,
                                    lua.protocolDir,
                                    wave.view.sampleFrequencyHz,
                                    createdAtMs,
                                    "当前协议元数据不完整，无法导出现场包",
                                    error)) {
        return false;
    }

    std::vector<std::uint8_t> rawCaptureBytes;
    if (!plot::encodeRawCaptureFile(capture, rawCaptureBytes, error)) {
        return false;
    }

    const auto& transfer = dockStore_.receiveState();
    const auto& hostLog = dockStore_.logState();
    const auto& scriptLog = dockStore_.scriptState();
    const auto& requestTrace = dockStore_.requestTraceState();
    const auto logSummary = buildSessionLogSummary(SessionLogSummaryCounts{
        .transferRows = transfer.rows.size(),
        .transferFrameRows = transfer.frameRows.size(),
        .hostLogRows = hostLog.rows.size(),
        .scriptLogRows = scriptLog.rows.size(),
        .requestTraceRows = requestTrace.rows.size(),
        .rawCaptureEvents = capture.events.size(),
        .rawCaptureBytes = capture.payload.size(),
    });
    const auto hostLogRows = copyReceiveRows(hostLog.rows);
    const auto scriptLogRows = copyReceiveRows(scriptLog.rows);
    const auto requestTraceRows = copyRequestTraceRows(requestTrace.rows);
    const auto hostLogText = dock::formatReceiveRowsText(hostLogRows, hostLog.showTimestamps, true);
    const auto scriptLogText = dock::formatReceiveRowsText(scriptLogRows, scriptLog.showTimestamps, true);
    const auto requestTraceText = dock::formatRequestTraceRowsCsv(requestTraceRows, requestTrace.showTimestamps);
    const auto analysisMarkersYaml = encodeAnalysisMarkersYaml(wave.analysisMarkers);

    std::vector<session::SessionPackageEntry> protocolEntries;
    if (!collectProtocolDirectoryEntries(lua.protocolDir, protocolEntries, error)) {
        return false;
    }

    auto package = buildSessionPackage(createdAtMs,
                                       std::move(configYaml),
                                       std::move(rawCaptureBytes),
                                       std::move(protocolEntries),
                                       analysisMarkersYaml,
                                       logSummary,
                                       hostLogText,
                                       scriptLogText,
                                       requestTraceText,
                                       lua.protocolName,
                                       lua.protocolDir,
                                       capture.sampleFrequencyHz);
    return session::writeSessionPackage(path, package, error);
}


bool Application::importSessionPackage(const std::filesystem::path& path, std::string& error)
{
    const auto package = session::readSessionPackage(path, error);
    return package && applySessionPackage(*package, true, error);
}


bool Application::prepareSessionScriptConfig(const session::SessionPackageData& package, std::string& error)
{
    const auto* entry = session::findSessionPackageEntry(package, "config.yaml");
    if (!entry) { error = "现场包缺少 config.yaml"; return false; }
    const auto loaded = configStore_.loadText(stringFromBytes(entry->bytes));
    if (!loaded.error.empty()) { error = loaded.error; return false; }
    // 准备只配置 worker，不应用会不可逆裁剪旧波形/记录的展示限制。
    configureScriptWorker(loaded.config);
    return true;
}

bool Application::restoreSessionPackageContext(const session::SessionPackageData& package,
                                               const bool allowMissingProtocol,
                                               std::vector<plot::WaveAnalysisMarker>& markers,
                                               std::string& error)
{
    if (!validateSessionPackageEntries(package, error)) return false;
    const auto* configEntry = session::findSessionPackageEntry(package, "config.yaml");
    if (configEntry == nullptr) {
        error = "现场包缺少 config.yaml";
        return false;
    }
    auto previousConfig = runtimeConfig_;
    previousConfig.protocol.selectedDir = dockStore_.luaState().protocolDir;
    auto loaded = configStore_.loadText(stringFromBytes(configEntry->bytes));
    if (!loaded.error.empty()) {
        error = loaded.error;
        return false;
    }
    loaded.config.configPath = runtimeConfig_.configPath;
    loaded.config.gui.theme = previousConfig.gui.theme;
    loaded.config.gui.fileDialogs = previousConfig.gui.fileDialogs;
    // 历史切换策略/范围是本机展示偏好，现场包恢复不得覆盖。
    loaded.config.gui.historyOnParsedSwitch = previousConfig.gui.historyOnParsedSwitch;
    loaded.config.gui.historyScope = previousConfig.gui.historyScope;
    loaded.config.gui.importProtocolSource = previousConfig.gui.importProtocolSource;
    loaded.config.gui.replayRawHistoryOnSchemaSwitch = previousConfig.gui.replayRawHistoryOnSchemaSwitch;
    // 不使用包内 config.yaml 的机器路径；所选协议由安全提取和显式加载决定。
    const auto persistentProtocolConfig = loaded.config.protocol;
    loaded.config.protocol = previousConfig.protocol;
    applyingImportContext_ = true;
    // 协议由导入准备阶段显式加载；配置应用不能按已更新的 UI 路径误判 unchanged。
    const bool applied = applyConfig(loaded.config, false);
    applyingImportContext_ = false;
    if (!applied && !allowMissingProtocol) {
        error = "应用现场包配置失败";
        applyingImportContext_ = true;
        static_cast<void>(applyConfig(previousConfig));
        applyingImportContext_ = false;
        return false;
    }
    captureProtocolConfigOverride_ = persistentProtocolConfig;
    return decodeSessionAnalysisMarkers(package, markers, error);
}


bool Application::applySessionPackage(const session::SessionPackageData& packageData, bool restoreCapture,
                                      std::string& error, bool allowMissingProtocol)
{
    const auto* package = &packageData;
    if (!validateOfflineReplayTransport(error)) return false;
    if (dataTransferStatus().active || retiringImportProtocolLoad_.valid()) {
        error = "导入或已取消的协议准备尚未完成";
        return false;
    }
    if (!validateSessionPackageEntries(*package, error)) return false;
    // 文件元数据必须在恢复配置及其历史上限之前校验，避免失败导入裁剪旧数据。
    std::optional<plot::RawCaptureFileData> rawCapture;
    if (restoreCapture && !decodeSessionRawCaptureEntry(*package, std::nullopt, rawCapture, error)) return false;
    if (rawCapture && !validateImportedFrequency(*rawCapture, error)) return false;
    std::string selectedProtocol;
    std::filesystem::path extractedProtocol;
    if (!selectSessionImportProtocol(*package, selectedProtocol, extractedProtocol, error)) return false;
    if (!extractedProtocol.empty()) importedProtocolDirectories_.push_back(extractedProtocol);
    auto previousConfig = runtimeConfig_;
    previousConfig.protocol.selectedDir = dockStore_.luaState().protocolDir;
    std::vector<plot::WaveAnalysisMarker> importedMarkers;
    if (!prepareSessionScriptConfig(*package, error)) return false;
    if (!selectedProtocol.empty()) {
        // 同步入口与 GUI 使用同一选择规则；强制实际加载，绝不依赖 applyConfig 的 unchanged 判断。
        const auto result = scriptWorker_.loadProtocolDirectory(selectedProtocol);
        // 同步入口也须等发布完成再丢弃初始化绘图，load future 先于 outputs 发布就绪。
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!scriptWorker_.inputIdle()) {
            static_cast<void>(scriptWorker_.drainOutputs());
            if (std::chrono::steady_clock::now() >= deadline) {
                configureScriptWorker(previousConfig);
                error = "所选导入协议初始化输出等待超时，未提交导入数据";
                return false;
            }
            std::this_thread::yield();
        }
        static_cast<void>(scriptWorker_.drainOutputs());
        if (!result.ok) {
            configureScriptWorker(previousConfig);
            error = "所选导入协议加载失败: " + result.lastError;
            return false;
        }
        auto& lua = dockStore_.luaState();
        lua.protocolDir = selectedProtocol;
        lua.protocolName = configStore_.protocolName(selectedProtocol);
        lua.scriptPath = configStore_.mainLuaPath(selectedProtocol).generic_string();
        lua.loaded = true;
        lua.lastError.clear();
        applyLuaScriptSnapshot(result.snapshot);
        static_cast<void>(scriptWorker_.drainOutputs());
        resetTransferFrameParser();
    }
    if (!restoreSessionPackageContext(*package, allowMissingProtocol, importedMarkers, error)) return false;
    if (rawCapture.has_value()) {
        if (rawCapture->waveform) {
            if (!importWaveCsvData(*rawCapture->waveform, error) || !importRawRecords(*rawCapture, error)) return false;
        } else if (!loadRawCaptureReplayTimeline(*rawCapture,
                                                 OfflineReplayProcessing::BrowseRaw,
                                                 OfflineReplayPacing::OriginalTimeline,
                                                 error,
                                                 package)) {
            return false;
        }
    }
    dockStore_.waveState().analysisMarkers = std::move(importedMarkers);
    setStatusMessage("现场包已载入，已初始化所选协议；默认仅浏览原始记录，不重算快照");
    loggingFacade_.info("session", "session loaded in browse-raw mode");
    return true;
}


bool Application::startRawCaptureRecording(const std::filesystem::path& path, std::string& error)
{
    if (rawCaptureRecording_.isOpen()) {
        error = "已有完整原始数据录制正在进行";
        return false;
    }

    const auto& luaState = dockStore_.luaState();
    const auto& wave = dockStore_.waveState();
    plot::RawCaptureFileData metadata{
        .protocolName = luaState.protocolName,
        .protocolDir = luaState.protocolDir,
        .sampleFrequencyHz = wave.view.sampleFrequencyHz,
        .capturedAtMs = nowMs(),
        .truncated = false,
        .payload = {},
        .events = {},
    };
    if (dataTransferStatus().active) { error = "数据任务进行中，不能开始录制"; return false; }

    if (!rawCaptureRecording_.open(path, metadata, error)) {
        return false;
    }
    if (const auto setup = currentPlotSetupSnapshot(wave, "recording_start"); setup.has_value()) {
        const auto event = makeRawCapturePlotSetupEvent(metadata.capturedAtMs, *setup);
        if (!rawCaptureRecording_.appendEvent(event, error)) {
            std::string closeError;
            static_cast<void>(rawCaptureRecording_.close(closeError));
            return false;
        }
    }
    setStatusMessage("完整原始数据录制已开始: " + path.generic_string());
    loggingFacade_.info("raw_capture", "完整原始数据录制已开始: " + path.generic_string());
    return true;
}


bool Application::stopRawCaptureRecording(std::string& error)
{
    if (!rawCaptureRecording_.isOpen()) {
        return true;
    }

    const auto path = rawCaptureRecording_.path();
    const auto bytesWritten = rawCaptureRecording_.bytesWritten();
    if (!rawCaptureRecording_.close(error)) {
        return false;
    }

    setStatusMessage("完整原始数据录制已停止: " + path.generic_string() + " (" + std::to_string(bytesWritten) +
                     " bytes)");
    loggingFacade_.info("raw_capture", "完整原始数据录制已停止: " + path.generic_string());
    return true;
}


bool Application::isRawCaptureRecording() const
{
    return rawCaptureRecording_.isOpen();
}


const std::filesystem::path& Application::rawCaptureRecordingPath() const
{
    return rawCaptureRecording_.path();
}


std::uint64_t Application::rawCaptureRecordingBytes() const
{
    return rawCaptureRecording_.bytesWritten();
}


bool Application::applyScriptRuntimeProfileEvents(const scripting::ScriptRuntimeOutputBatch& batch)
{
    bool changed = false;
    for (const auto& profileEvent : batch.streamRuntimeProfiles) {
        std::string transferFrameError;
        if (!applyTransferFrameRuntimeProfileEvent(profileEvent, transferFrameError)) {
            loggingFacade_.warn("protocol", "收发记录帧解析器应用 runtime profile 失败: " + transferFrameError);
        }
        const auto timestampMs = activeConnection_.has_value() ? activeConnection_->timestampMs : nowMs();
        const plot::RawCaptureEvent event{
            .type =
                profileEvent.cleared ? plot::RawCaptureEventType::ProfileClear : plot::RawCaptureEventType::ProfileSet,
            .timestampMs = timestampMs,
            .bytes = {},
            .profile =
                plot::RawCaptureProfileEventData{
                    .frameName = profileEvent.frameName,
                    .length = profileEvent.length,
                    .channelMap = profileEvent.channelMap,
                },
            .plotSetup = {},
        };
        if (!suppressRawCaptureProfileEvents_) {
            appendRawCaptureEvent(event);
        }
        changed = true;
    }
    return changed;
}

} // namespace protoscope::app
