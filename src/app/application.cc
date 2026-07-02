#include "protoscope/app/application.hpp"

#include "protoscope/plot/wave_math.hpp"
#include "protoscope/protocol_utils/codec.hpp"
#include "protoscope/scripting/pipeline_threading.hpp"
#include "protoscope/session/session_package.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>

#include <yaml-cpp/yaml.h>

namespace protoscope::app {

namespace {

    constexpr std::size_t kRawCaptureReplayChunkBytes = 1024;
    constexpr std::size_t kRawCaptureReplaySeekNoticeEvents = 4096;
    constexpr std::size_t kTransportEventsPerPump = 256;
    constexpr auto kTransportEventBudget = std::chrono::milliseconds(4);
    constexpr std::uint64_t kCommPressureDebugLogIntervalMs = 2000;
    constexpr std::string_view kSessionProtocolEntryPrefix = "protocol/";

    std::uint64_t nowMs()
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    std::uint64_t nowUs()
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    double elapsedMilliseconds(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end)
    {
        return std::chrono::duration<double, std::milli>(end - start).count();
    }

    const char* stateMessage(transport::TransportState state)
    {
        switch (state) {
            case transport::TransportState::Closed:
                return "closed";
            case transport::TransportState::Opening:
                return "opening";
            case transport::TransportState::Open:
                return "open";
            case transport::TransportState::Error:
                return "error";
        }
        return "unknown";
    }

    const char* txRequestKindName(scripting::TxRequestKind kind)
    {
        switch (kind) {
            case scripting::TxRequestKind::Send:
                return "send";
            case scripting::TxRequestKind::Request:
                return "request";
        }
        return "send";
    }

    const char* txEventStateName(scripting::TxEventState state)
    {
        switch (state) {
            case scripting::TxEventState::Sent:
                return "sent";
            case scripting::TxEventState::Completed:
                return "completed";
            case scripting::TxEventState::Failed:
                return "failed";
            case scripting::TxEventState::Timeout:
                return "timeout";
            case scripting::TxEventState::Rejected:
                return "rejected";
            case scripting::TxEventState::Dropped:
                return "dropped";
            case scripting::TxEventState::Canceled:
                return "canceled";
        }
        return "rejected";
    }

    void appendLogField(std::ostringstream& out, std::string_view name, std::string_view value)
    {
        if (value.empty()) {
            return;
        }
        out << ' ' << name << '=' << value;
    }

    void appendLogField(std::ostringstream& out, std::string_view name, std::uint64_t value)
    {
        out << ' ' << name << '=' << value;
    }

    std::string payloadPreviewSuffix(const config::AppLoggingConfig& logging,
                                     const std::vector<std::uint8_t>& payload)
    {
        if (!logging.payloadPreview.enabled || payload.empty()) {
            return {};
        }
        std::ostringstream out;
        const auto maxBytes = logging.payloadPreview.maxBytes == 0U ? payload.size() : logging.payloadPreview.maxBytes;
        out << " payload_hex=" << protocol_utils::bytesToHex(
            std::vector<std::uint8_t>(payload.begin(),
                                      payload.begin() + static_cast<std::ptrdiff_t>(
                                                          (std::min)(payload.size(), maxBytes))),
            true);
        if (payload.size() > maxBytes) {
            out << "...";
        }
        return out.str();
    }

    std::string transportContextFields(std::string_view kind, const transport::ConnectionContext& context)
    {
        std::ostringstream out;
        appendLogField(out, "kind", kind);
        appendLogField(out, "endpoint", context.endpoint);
        appendLogField(out, "connection_id", context.connectionId);
        return out.str();
    }

    std::string txRequestLogMessage(std::string_view state,
                                    const scripting::TxRequest& request,
                                    std::size_t queueSize,
                                    std::optional<std::string_view> error = std::nullopt,
                                    std::uint64_t timestampMs = 0)
    {
        std::ostringstream out;
        appendLogField(out, "kind", txRequestKindName(request.kind));
        appendLogField(out, "state", state);
        appendLogField(out, "endpoint", request.connection.endpoint);
        appendLogField(out, "connection_id", request.connection.connectionId);
        appendLogField(out, "request_id", request.id);
        appendLogField(out, "tag", request.tag);
        appendLogField(out, "attempt", request.attempt);
        out << " bytes=" << request.payload.size();
        out << " queue_size=" << queueSize;
        if (timestampMs != 0U) {
            const auto elapsedMs = timestampMs >= request.createdAtMs ? timestampMs - request.createdAtMs : 0U;
            out << " elapsed_ms=" << elapsedMs;
        }
        if (error.has_value() && !error->empty()) {
            appendLogField(out, "error", *error);
        }
        return out.str();
    }

    bool sameColor(const std::optional<std::array<float, 4>>& left, const std::optional<std::array<float, 4>>& right)
    {
        if (left.has_value() != right.has_value()) {
            return false;
        }
        if (!left.has_value()) {
            return true;
        }
        for (std::size_t index = 0; index < left->size(); ++index) {
            if (std::abs((*left)[index] - (*right)[index]) > 1e-6F) {
                return false;
            }
        }
        return true;
    }

    bool sameLineWidth(const std::optional<float>& left, const std::optional<float>& right)
    {
        if (left.has_value() != right.has_value()) {
            return false;
        }
        if (!left.has_value()) {
            return true;
        }
        return std::abs(*left - *right) <= 1e-6F;
    }

    bool sameBitDisplayIdentity(const plot::BitDisplaySpec& left, const plot::BitDisplaySpec& right)
    {
        return left.enabled == right.enabled && left.firstBit == right.firstBit && left.bitCount == right.bitCount;
    }

    bool sameBitDisplaySpec(const plot::BitDisplaySpec& left, const plot::BitDisplaySpec& right)
    {
        return sameBitDisplayIdentity(left, right) && std::abs(left.yOffset - right.yOffset) <= 1e-12;
    }

    bool sameChannelSpecs(const std::vector<plot::ChannelSpec>& setupChannels, const plot::OscilloscopeBuffer& buffer)
    {
        if (setupChannels.size() != buffer.channelCount()) {
            return false;
        }
        for (std::size_t i = 0; i < setupChannels.size(); ++i) {
            const auto current = buffer.channelSpec(i);
            if (!current.has_value()) {
                return false;
            }
            const auto& setup = setupChannels[i];
            if (current->label != setup.label || current->unit != setup.unit ||
                !sameColor(current->color, setup.color) || !sameLineWidth(current->lineWidth, setup.lineWidth) ||
                !sameBitDisplayIdentity(current->bitDisplay, setup.bitDisplay)) {
                return false;
            }
        }
        return true;
    }

    bool sameFullChannelSpecs(const std::vector<plot::ChannelSpec>& setupChannels,
                              const plot::OscilloscopeBuffer& buffer)
    {
        if (!sameChannelSpecs(setupChannels, buffer)) {
            return false;
        }
        for (std::size_t index = 0; index < setupChannels.size(); ++index) {
            const auto current = buffer.channelSpec(index);
            const auto& setup = setupChannels[index];
            if (!current.has_value() || std::abs(current->ratio - setup.ratio) > 1e-12 ||
                std::abs(current->scale - setup.scale) > 1e-12 || std::abs(current->offset - setup.offset) > 1e-12 ||
                !sameBitDisplaySpec(current->bitDisplay, setup.bitDisplay)) {
                return false;
            }
        }
        return true;
    }

    bool sameChannelIdentity(const std::vector<plot::ChannelSpec>& setupChannels,
                             const plot::OscilloscopeBuffer& buffer)
    {
        if (setupChannels.size() != buffer.channelCount()) {
            return false;
        }
        for (std::size_t i = 0; i < setupChannels.size(); ++i) {
            const auto current = buffer.channelSpec(i);
            if (!current.has_value()) {
                return false;
            }
            const auto& setup = setupChannels[i];
            if (current->label != setup.label || current->unit != setup.unit ||
                !sameBitDisplayIdentity(current->bitDisplay, setup.bitDisplay)) {
                return false;
            }
        }
        return true;
    }

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

    bool writeFileBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes, std::string& error)
    {
        std::error_code directoryError;
        const auto parent = path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, directoryError);
            if (directoryError) {
                error = "创建目录失败: " + directoryError.message();
                return false;
            }
        }
        std::ofstream out(path, std::ios::binary);
        if (!out.good()) {
            error = "无法写入文件: " + path.generic_string();
            return false;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out.good()) {
            error = "写入文件失败: " + path.generic_string();
            return false;
        }
        return true;
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

    std::string csvEscape(std::string_view text)
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

    plot::RawCapturePlotSetupEventData toRawPlotSetup(const scripting::PlotSetup& setup)
    {
        plot::RawCapturePlotSetupEventData rawSetup;
        rawSetup.source = setup.source;
        rawSetup.view = setup.view;
        rawSetup.resetHistory = setup.resetHistory;
        rawSetup.channels.reserve(setup.channels.size());
        for (const auto& channel : setup.channels) {
            rawSetup.channels.push_back(plot::ChannelSpec{
                .label = channel.label,
                .unit = channel.unit,
                .ratio = channel.ratio,
                .scale = channel.scale,
                .offset = channel.offset,
                .color = channel.color,
                .lineWidth = channel.lineWidth,
                .bitDisplay = channel.bitDisplay,
            });
        }
        return rawSetup;
    }

    using ScriptPlotAppendRequest = std::pair<std::size_t, plot::WaveAppendRequest>;

    struct ScriptPlotAppendDrainState {
        std::vector<ScriptPlotAppendRequest> selected;
        std::vector<ScriptPlotAppendRequest> deferred;
        std::unordered_map<std::string, std::size_t> selectedIndexes;
        std::size_t maxSelectedKeys{0};
    };

    std::string scriptPlotAppendKey(std::size_t channelIndex, const std::string& source)
    {
        auto key = std::to_string(channelIndex);
        key.push_back('\x1F');
        key.append(source);
        return key;
    }

    ScriptPlotAppendRequest takePendingScriptPlotAppend(std::deque<ScriptPlotAppendRequest>& pending)
    {
        auto append = std::move(pending.front());
        pending.pop_front();
        return append;
    }

    void mergePlotAppendSamples(plot::WaveAppendRequest& target, plot::WaveAppendRequest& source)
    {
        target.samples.insert(target.samples.end(),
                              std::make_move_iterator(source.samples.begin()),
                              std::make_move_iterator(source.samples.end()));
    }

    bool shouldDeferNewScriptPlotAppend(const ScriptPlotAppendDrainState& state, const std::string& key)
    {
        return !state.selectedIndexes.contains(key) && state.selectedIndexes.size() >= state.maxSelectedKeys;
    }

    void deferScriptPlotAppend(ScriptPlotAppendDrainState& state, ScriptPlotAppendRequest& append)
    {
        auto& [channelIndex, request] = append;
        state.deferred.emplace_back(channelIndex, std::move(request));
    }

    void selectOrMergeScriptPlotAppend(ScriptPlotAppendDrainState& state,
                                       ScriptPlotAppendRequest& append,
                                       std::string key)
    {
        auto& [channelIndex, request] = append;
        const auto [position, inserted] = state.selectedIndexes.emplace(std::move(key), state.selected.size());
        if (inserted) {
            state.selected.emplace_back(channelIndex, std::move(request));
            return;
        }
        mergePlotAppendSamples(state.selected[position->second].second, request);
    }

    void drainScriptPlotAppendCandidate(ScriptPlotAppendDrainState& state, ScriptPlotAppendRequest append)
    {
        auto& [channelIndex, request] = append;
        const auto key = scriptPlotAppendKey(channelIndex, request.source);
        if (shouldDeferNewScriptPlotAppend(state, key)) {
            deferScriptPlotAppend(state, append);
            return;
        }
        selectOrMergeScriptPlotAppend(state, append, key);
    }

    void restoreDeferredScriptPlotAppends(std::deque<ScriptPlotAppendRequest>& pending,
                                          std::vector<ScriptPlotAppendRequest>& deferred)
    {
        for (auto& append : deferred) {
            pending.push_back(std::move(append));
        }
    }

    std::vector<ScriptPlotAppendRequest> mergeNonEmptyScriptPlotAppends(std::vector<ScriptPlotAppendRequest> requests)
    {
        std::vector<ScriptPlotAppendRequest> mergedRequests;
        std::unordered_map<std::string, std::size_t> mergedIndexes;
        mergedRequests.reserve(requests.size());
        for (auto append : requests) {
            auto& [channelIndex, request] = append;
            if (request.samples.empty()) {
                continue;
            }
            const auto key = scriptPlotAppendKey(channelIndex, request.source);
            const auto [position, inserted] = mergedIndexes.emplace(key, mergedRequests.size());
            if (inserted) {
                mergedRequests.emplace_back(channelIndex, std::move(request));
                continue;
            }
            mergePlotAppendSamples(mergedRequests[position->second].second, request);
        }
        return mergedRequests;
    }

    bool nearlyEqual(double left, double right);
    bool sameWaveViewState(const plot::WaveViewState& view, const plot::ViewConfig& config);

    bool sameAppliedPlotSetup(const plot::WaveDockState& wave, const plot::RawCapturePlotSetupEventData& setup)
    {
        const auto currentConfig = wave.buffer.viewConfig();
        return sameFullChannelSpecs(setup.channels, wave.buffer) &&
               nearlyEqual(currentConfig.timeScale, setup.view.timeScale) &&
               currentConfig.timeUnit == setup.view.timeUnit &&
               nearlyEqual(currentConfig.verticalMin, setup.view.verticalMin) &&
               nearlyEqual(currentConfig.verticalMax, setup.view.verticalMax) &&
               currentConfig.verticalUnit == setup.view.verticalUnit &&
               currentConfig.historyLimit == setup.view.historyLimit && sameWaveViewState(wave.view, setup.view);
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
        const std::vector<plot::RawCaptureEvent>& events, std::uint64_t keepStart)
    {
        std::vector<plot::RawCaptureEvent> activeProfiles;
        std::optional<plot::RawCaptureEvent> activePlotSetup;
        std::vector<plot::RawCaptureEvent> keptEvents;
        std::uint64_t rxCursor = 0;

        for (const auto& event : events) {
            if (event.type != plot::RawCaptureEventType::RxBytes) {
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
            error = std::string(metadataError);
            return false;
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

    std::vector<const session::SessionPackageEntry*> collectSessionProtocolEntries(
        const session::SessionPackageData& package)
    {
        std::vector<const session::SessionPackageEntry*> protocolEntries;
        for (const auto& entry : package.entries) {
            if (entry.name.starts_with(kSessionProtocolEntryPrefix)) {
                protocolEntries.push_back(&entry);
            }
        }
        return protocolEntries;
    }

    bool releaseSessionProtocolEntries(const std::vector<const session::SessionPackageEntry*>& protocolEntries,
                                       const std::filesystem::path& protocolDir,
                                       std::string& error)
    {
        for (const auto* entry : protocolEntries) {
            const auto relativeName = entry->name.substr(kSessionProtocolEntryPrefix.size());
            const auto outputPath = protocolDir / std::filesystem::path(relativeName);
            if (!writeFileBytes(outputPath, entry->bytes, error)) {
                const auto writeError = error;
                error = "释放现场包协议目录失败: " + writeError;
                return false;
            }
        }
        return true;
    }

    bool validateReleasedProtocolDirectory(const std::filesystem::path& protocolDir,
                                           const scripting::FileIoConfig& fileIoConfig,
                                           std::string& error)
    {
        std::error_code protocolEntryError;
        const auto mainLuaPath = protocolDir / "main.lua";
        if (!std::filesystem::is_regular_file(mainLuaPath, protocolEntryError) || protocolEntryError) {
            error = "现场包缺少 protocol/main.lua";
            if (protocolEntryError) {
                error += ": " + protocolEntryError.message();
            }
            return false;
        }

        scripting::ScriptHost probeHost;
        probeHost.setFileIoConfig(fileIoConfig);
        if (!probeHost.loadProtocolDirectory(protocolDir.generic_string())) {
            error = "现场包协议脚本无效: " + probeHost.lastError();
            return false;
        }
        return true;
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

    std::string transferFrameFieldValueText(const scripting::StreamFieldValue& value)
    {
        return std::visit(
            [](const auto& stored) -> std::string {
                using ValueType = std::decay_t<decltype(stored)>;
                std::ostringstream builder;
                if constexpr (std::is_same_v<ValueType, std::vector<std::uint8_t>>) {
                    builder << protocol_utils::bytesToHex(stored, true);
                } else if constexpr (std::is_same_v<ValueType, std::vector<std::int64_t>> ||
                                     std::is_same_v<ValueType, std::vector<double>>) {
                    builder << "[";
                    for (std::size_t index = 0; index < stored.size(); ++index) {
                        if (index > 0) {
                            builder << ", ";
                        }
                        builder << stored[index];
                    }
                    builder << "]";
                } else if constexpr (std::is_same_v<ValueType, double>) {
                    builder << std::setprecision(6) << stored;
                } else {
                    builder << stored;
                }
                return builder.str();
            },
            value.value);
    }

    std::string transferFrameMessage(const scripting::StreamParsedFrame& frame)
    {
        std::ostringstream builder;
        builder << "frame";
        if (!frame.name.empty()) {
            builder << " " << frame.name;
        }
        builder << " len=" << frame.raw.size();
        if (!frame.fields.empty()) {
            builder << " fields={";
            bool first = true;
            for (const auto& [name, value] : frame.fields) {
                if (!first) {
                    builder << ", ";
                }
                first = false;
                builder << name << "=" << transferFrameFieldValueText(value);
            }
            builder << "}";
        }
        return builder.str();
    }

    transport::TransportTxKind toTransportTxKind(scripting::TxRequestKind kind)
    {
        switch (kind) {
            case scripting::TxRequestKind::Send:
                return transport::TransportTxKind::Send;
            case scripting::TxRequestKind::Request:
                return transport::TransportTxKind::Request;
        }
        return transport::TransportTxKind::Send;
    }

    scripting::TxEventState toScriptTxState(transport::TransportTxState state)
    {
        switch (state) {
            case transport::TransportTxState::Sent:
                return scripting::TxEventState::Sent;
            case transport::TransportTxState::Timeout:
                return scripting::TxEventState::Timeout;
            case transport::TransportTxState::Rejected:
                return scripting::TxEventState::Rejected;
            case transport::TransportTxState::Dropped:
                return scripting::TxEventState::Dropped;
            case transport::TransportTxState::Canceled:
                return scripting::TxEventState::Canceled;
        }
        return scripting::TxEventState::Rejected;
    }

    bool isGuardedTerminalFailure(const scripting::TxRequest& request, scripting::TxEventState state)
    {
        if (!request.guarded) {
            return false;
        }
        switch (state) {
            case scripting::TxEventState::Failed:
            case scripting::TxEventState::Rejected:
            case scripting::TxEventState::Dropped:
                return true;
            case scripting::TxEventState::Timeout:
                return request.attempt >= request.maxAttempts;
            default:
                return false;
        }
    }

    std::optional<std::string> guardStateForEvent(const scripting::TxRequest& request, scripting::TxEventState state)
    {
        if (!request.guarded) {
            return std::nullopt;
        }
        if (state == scripting::TxEventState::Timeout && request.attempt < request.maxAttempts) {
            return std::string("retrying");
        }
        if (isGuardedTerminalFailure(request, state)) {
            return std::string("halted");
        }
        return std::string("active");
    }

    dock::RequestTraceKind toRequestTraceKind(scripting::TxRequestKind kind)
    {
        switch (kind) {
            case scripting::TxRequestKind::Send:
                return dock::RequestTraceKind::Send;
            case scripting::TxRequestKind::Request:
                return dock::RequestTraceKind::Request;
        }
        return dock::RequestTraceKind::Send;
    }

    dock::RequestTraceState toRequestTraceState(scripting::TxEventState state)
    {
        switch (state) {
            case scripting::TxEventState::Sent:
                return dock::RequestTraceState::Sent;
            case scripting::TxEventState::Completed:
                return dock::RequestTraceState::Completed;
            case scripting::TxEventState::Failed:
                return dock::RequestTraceState::Failed;
            case scripting::TxEventState::Timeout:
                return dock::RequestTraceState::Timeout;
            case scripting::TxEventState::Rejected:
                return dock::RequestTraceState::Rejected;
            case scripting::TxEventState::Dropped:
                return dock::RequestTraceState::Dropped;
            case scripting::TxEventState::Canceled:
                return dock::RequestTraceState::Canceled;
        }
        return dock::RequestTraceState::Rejected;
    }

    std::string guardStateForTrace(const scripting::TxRequest& request, dock::RequestTraceState state)
    {
        if (!request.guarded) {
            return {};
        }
        if (state == dock::RequestTraceState::Queued) {
            return "queued";
        }
        const auto txState = [&]() -> std::optional<scripting::TxEventState> {
            switch (state) {
                case dock::RequestTraceState::Sent:
                    return scripting::TxEventState::Sent;
                case dock::RequestTraceState::Completed:
                    return scripting::TxEventState::Completed;
                case dock::RequestTraceState::Failed:
                    return scripting::TxEventState::Failed;
                case dock::RequestTraceState::Timeout:
                    return scripting::TxEventState::Timeout;
                case dock::RequestTraceState::Rejected:
                    return scripting::TxEventState::Rejected;
                case dock::RequestTraceState::Dropped:
                    return scripting::TxEventState::Dropped;
                case dock::RequestTraceState::Canceled:
                    return scripting::TxEventState::Canceled;
                default:
                    return std::nullopt;
            }
        }();
        if (!txState.has_value()) {
            return {};
        }
        const auto guardState = guardStateForEvent(request, *txState);
        return guardState.value_or(std::string{});
    }

    dock::RequestTraceRow makeRequestTraceRow(const scripting::TxRequest& request,
                                              dock::RequestTraceState state,
                                              const std::optional<std::string>& error,
                                              std::uint64_t timestampMs)
    {
        return dock::RequestTraceRow{
            .timestampMs = timestampMs,
            .id = request.id,
            .kind = toRequestTraceKind(request.kind),
            .state = state,
            .endpoint = request.connection.endpoint,
            .tag = request.tag,
            .bytes = request.payload.size(),
            .queuedMs = request.createdAtMs,
            .finishedMs = timestampMs,
            .timeoutMs = request.timeoutMs,
            .durationMs = timestampMs >= request.createdAtMs ? timestampMs - request.createdAtMs : 0,
            .guarded = request.guarded,
            .attempt = request.attempt,
            .maxAttempts = request.maxAttempts,
            .guardState = guardStateForTrace(request, state),
            .error = error.value_or(std::string{}),
        };
    }

    bool nearlyEqual(double left, double right)
    {
        return std::abs(left - right) <= 1e-12;
    }

    std::string formatFrequencyInput(double valueHz)
    {
        if (!(std::isfinite(valueHz)) || valueHz <= 0.0) {
            return {};
        }
        std::ostringstream out;
        out << valueHz;
        return out.str();
    }

    bool sameWaveViewState(const plot::WaveViewState& view, const plot::ViewConfig& config)
    {
        const double defaultVisibleDuration =
            (std::max)(view.minVisibleTimeSpan, (std::max)(config.timeScale * 1000.0, config.timeScale));
        if (!nearlyEqual(view.visibleDuration, defaultVisibleDuration)) {
            return false;
        }
        if (!nearlyEqual(view.manualVerticalMin, config.verticalMin) ||
            !nearlyEqual(view.manualVerticalMax, config.verticalMax)) {
            return false;
        }
        return true;
    }

} // namespace

Application::Application() = default;

bool Application::initialize()
{
    loggingFacade_.bindDockStore(&dockStore_);

    std::error_code configDirectoryError;
    std::filesystem::create_directories(configStore_.defaultConfigPath().parent_path(), configDirectoryError);
    if (configDirectoryError) {
        const std::string message = "初始化 config 工作目录失败: " + configDirectoryError.message();
        dockStore_.markDirty(message);
        loggingFacade_.warn("config", message);
    }

    std::string workspaceError;
    if (!configStore_.ensureDefaultProtocolWorkspace(workspaceError)) {
        dockStore_.markDirty("初始化 protocols 工作目录失败: " + workspaceError);
        loggingFacade_.warn("config", "初始化 protocols 工作目录失败: " + workspaceError);
    }

    const auto loaded = configStore_.load(configStore_.defaultConfigPath());
    runtimeConfig_ = loaded.config;
    loadedConfigFromDisk_ = loaded.loadedFromDisk;
    loggingFacade_.applyConfig(loaded.config.logging);
    const bool configApplied = applyConfig(loaded.config);

    auto& configState = dockStore_.configState();
    configState.loadedFromPath = loaded.resolvedPath.generic_string();
    configState.fileTimestampMs = configStore_.snapshot(loaded.resolvedPath).timestampMs;
    if (!configApplied) {
        const auto& lua = dockStore_.luaState();
        const auto message = lua.lastError.empty() ? std::string("协议加载失败") : "协议加载失败: " + lua.lastError;
        dockStore_.markDirty(message);
    } else if (loaded.loadedFromDisk) {
        dockStore_.clearDirty("已从 YAML 加载配置");
    } else if (!loaded.error.empty()) {
        dockStore_.markDirty(loaded.error);
    } else if (workspaceError.empty() && !configDirectoryError) {
        dockStore_.markDirty("未找到配置文件，已使用默认配置");
    }

    syncDockState();
    return true;
}

bool Application::applyConfig(const config::AppConfig& config)
{
    captureProtocolConfigOverride_.reset();
    runtimeConfig_ = config;
    resetStreamBufferAlertState();
    const auto postprocessWorkerThreads = scripting::resolvePipelineWorkerThreads(
        config.scripting.pipeline.workerThreads, std::thread::hardware_concurrency());
    scriptWorker_.configure(scripting::ScriptRuntimeWorkerConfig{
        .enabled = config.scripting.workerEnabled,
        .postprocessWorkerThreads = postprocessWorkerThreads,
        .rxQueueLimitBytes = config.scripting.workerRxQueueLimitBytes,
        .memoryBudgetBytes = config.scripting.workerMemoryBudgetBytes,
        .memoryBudgetAvailableRatio = config.scripting.workerMemoryBudgetAvailableRatio,
        .outputQueueLimit = config.scripting.workerOutputQueueLimit,
        .batchBytes = config.scripting.workerBatchBytes,
        .backpressureEnabled = config.scripting.workerBackpressureEnabled,
        .backpressureHighWatermark = config.scripting.workerBackpressureHighWatermark,
        .backpressureLowWatermark = config.scripting.workerBackpressureLowWatermark,
    });
    scriptWorker_.setFileIoConfig(config.scripting.fileIo);
    applyHistoryLimits(config.gui.logHistory);
    dockStore_.waveState().buffer.setMaxTotalSamples(config.gui.wave.maxTotalSamples);
    dockStore_.waveState().buffer.setResetHistoryOnTimeReset(config.gui.wave.resetHistoryOnTimeReset);
    configStore_.applyToDock(config, dockStore_);
    loggingFacade_.applyConfig(config.logging);
    return reloadProtocolDirectory(dockStore_.luaState().protocolDir);
}

void Application::setLogLevel(const config::LogLevel level)
{
    runtimeConfig_.logging.level = level;
    auto logging = loggingFacade_.currentConfig();
    logging.level = level;
    // 核心流程：菜单切换只刷新日志门限，不走 applyConfig，避免无关协议重载。
    loggingFacade_.applyConfig(logging);
}

config::AppConfig Application::captureConfig() const
{
    auto captured = configStore_.captureFromDock(dockStore_);
    if (captureProtocolConfigOverride_.has_value()) {
        captured.protocol.rootDir = captureProtocolConfigOverride_->rootDir;
        captured.protocol.selectedDir = captureProtocolConfigOverride_->selectedDir;
    }
    captured.performance = runtimeConfig_.performance;
    captured.gui.window = runtimeConfig_.gui.window;
    captured.gui.rendererBackend = runtimeConfig_.gui.rendererBackend;
    captured.gui.logHistory = runtimeConfig_.gui.logHistory;
    captured.gui.rawCapture = runtimeConfig_.gui.rawCapture;
    captured.gui.realtimeBacklog = runtimeConfig_.gui.realtimeBacklog;
    captured.gui.elfSymbolCombo = runtimeConfig_.gui.elfSymbolCombo;
    captured.gui.wave.resetHistoryOnTimeReset = runtimeConfig_.gui.wave.resetHistoryOnTimeReset;
    captured.gui.showAppHeader = runtimeConfig_.gui.showAppHeader;
    captured.gui.sendHistoryLimit = runtimeConfig_.gui.sendHistoryLimit;
    captured.gui.luaDockLayoutDebug = runtimeConfig_.gui.luaDockLayoutDebug;
    captured.gui.replayRawHistoryOnSchemaSwitch = runtimeConfig_.gui.replayRawHistoryOnSchemaSwitch;
    captured.protocol.tx = runtimeConfig_.protocol.tx;
    captured.app.language = runtimeConfig_.app.language;
    captured.receive = runtimeConfig_.receive;
    captured.scripting = runtimeConfig_.scripting;
    captured.logging = loggingFacade_.currentConfig();
    return captured;
}

const config::AppConfig& Application::runtimeConfig() const
{
    return runtimeConfig_;
}

bool Application::loadedConfigFromDisk() const
{
    return loadedConfigFromDisk_;
}

bool Application::reloadProtocolDirectory(const std::string& protocolDir, bool forceReload)
{
    auto& lua = dockStore_.luaState();
    try {
        const auto resolvedDir = configStore_.normalizeProtocolDir(lua.protocolRootDir, protocolDir);
        const auto resolvedDirText = resolvedDir.generic_string();
        const auto protocolName = configStore_.protocolName(resolvedDir);
        const auto scriptPath = configStore_.mainLuaPath(resolvedDir).generic_string();
        const bool unchanged = lua.loaded && lua.protocolDir == resolvedDirText && lua.scriptPath == scriptPath;

        // 核心流程：配置热加载只在协议目录真正变化时重载脚本，避免窗口刷新阶段重复刷加载日志。
        if (!forceReload && unchanged) {
            loggingFacade_.trace("protocol", "protocol reload skipped kind=protocol endpoint=" + resolvedDirText);
            lua.lastError.clear();
            applyLuaScriptSnapshot(scriptWorker_.snapshot());
            return true;
        }

        loggingFacade_.info("protocol", "protocol reload requested kind=protocol endpoint=" + resolvedDirText);
        if (!probeProtocolDirectory(resolvedDirText)) {
            return false;
        }

        prepareProtocolRuntimeReload();
        scriptWorker_.setFileIoConfig(runtimeConfig_.scripting.fileIo);
        const auto loadResult = scriptWorker_.loadProtocolDirectory(resolvedDirText);
        if (!applyProtocolLoadResult(resolvedDirText, protocolName, scriptPath, loadResult)) {
            return false;
        }
        captureProtocolConfigOverride_.reset();
        return true;
    } catch (const std::exception& ex) {
        lua.lastError = std::string("协议重载异常: ") + ex.what();
    } catch (...) {
        lua.lastError = "协议重载异常: 未知异常";
    }

    loggingFacade_.error("protocol", lua.lastError);
    applyLuaScriptSnapshot(scriptWorker_.snapshot());
    return false;
}

void Application::applyLuaScriptSnapshot(const scripting::ScriptRuntimeSnapshot& snapshot)
{
    auto& lua = dockStore_.luaState();
    lua.docks = snapshot.docks;
    lua.controls = snapshot.controls;
    lua.controlStates = snapshot.controlStates;
}

bool Application::probeProtocolDirectory(const std::string& resolvedDirText)
{
    scripting::ScriptHost probeHost;
    probeHost.setFileIoConfig(runtimeConfig_.scripting.fileIo);
    if (probeHost.loadProtocolDirectory(resolvedDirText)) {
        return true;
    }

    auto& lua = dockStore_.luaState();
    lua.lastError = probeHost.lastError();
    loggingFacade_.error("protocol", "协议加载探测失败: " + lua.lastError);
    applyLuaScriptSnapshot(scriptWorker_.snapshot());
    return false;
}

void Application::prepareProtocolRuntimeReload()
{
    try {
        cancelAllTxRequests("协议已重新加载");
    } catch (const std::exception& ex) {
        loggingFacade_.warn("protocol", std::string("协议重载前取消旧请求失败: ") + ex.what());
    } catch (...) {
        loggingFacade_.warn("protocol", "协议重载前取消旧请求失败: 未知异常");
    }
    try {
        // 核心流程：取消旧 request 可能触发旧脚本 on_tx；替换宿主前丢弃旧输出，
        // 避免旧回调追加的新请求、状态或弹窗污染新协议运行态。
        static_cast<void>(scriptWorker_.drainOutputs());
    } catch (const std::exception& ex) {
        loggingFacade_.warn("protocol", std::string("协议重载前清理旧脚本输出失败: ") + ex.what());
    } catch (...) {
        loggingFacade_.warn("protocol", "协议重载前清理旧脚本输出失败: 未知异常");
    }
}

bool Application::applyProtocolLoadResult(const std::string& resolvedDirText,
                                          const std::string& protocolName,
                                          const std::string& scriptPath,
                                          const scripting::ScriptRuntimeLoadResult& loadResult)
{
    auto& lua = dockStore_.luaState();
    if (!loadResult.ok) {
        lua.lastError = loadResult.lastError;
        loggingFacade_.error("protocol", "协议加载失败: " + lua.lastError);
        applyLuaScriptSnapshot(loadResult.snapshot);
        return false;
    }

    lua.protocolDir = resolvedDirText;
    lua.protocolName = protocolName;
    lua.scriptPath = scriptPath;
    lua.loaded = true;
    applyLuaScriptSnapshot(loadResult.snapshot);
    lua.lastError.clear();
    refreshTransferFrameDisplayAfterProtocolReload();
    loggingFacade_.info("protocol", "protocol loaded kind=protocol endpoint=" + resolvedDirText);
    flushScriptOutputs();
    syncDockState();
    return true;
}

void Application::refreshTransferFrameDisplayAfterProtocolReload()
{
    if (dockStore_.receiveState().displayMode == dock::TransferLogDisplayMode::ParsedFrames) {
        rebuildTransferFrameRows();
    } else {
        resetTransferFrameDisplayState();
    }
}

bool Application::pumpOnce()
{
    bool changed = false;
    const bool hadPendingScriptPlotAppends = !pendingScriptPlotAppends_.empty();
    changed = handleTransportEvents() || changed;
    std::string replayError;
    const bool replayChanged = pumpRawCaptureReplay(replayError);
    changed = replayChanged || changed;
    if (!replayChanged && !replayError.empty()) {
        loggingFacade_.error("wave", replayError);
        dockStore_.waveState().statusMessage = replayError;
    }
    scriptWorker_.postTick(nowMs());
    changed = flushScriptOutputs() || changed;
    changed = processRequestTimeouts() || changed;
    changed = flushScriptOutputs() || changed;
    if (hadPendingScriptPlotAppends && !pendingScriptPlotAppends_.empty()) {
        changed = applyScriptOutputBatch(scripting::ScriptRuntimeOutputBatch{}) || changed;
    }
    changed = flushPendingTransferFrameRows(transferFrameRowsPerPump()) || changed;
    syncDockState();
    return changed;
}

void Application::shutdown()
{
    if (rawCaptureReplay_.loaded) {
        cancelRawCaptureImportReplay();
        rawCaptureReplay_ = RawCaptureReplayState{};
    }
    closeTransport();
    scriptWorker_.stop();
}

dock::DockStore& Application::docks()
{
    return dockStore_;
}

const dock::DockStore& Application::docks() const
{
    return dockStore_;
}

void Application::openTransport()
{
    cancelAllTxRequests("连接重新打开");
    pendingTransportEvents_.clear();
    pendingRxByteChunks_.clear();
    const auto kind = dockStore_.commState().kind;
    transport_ = createTransport(kind);
    if (!transport_) {
        dockStore_.commState().lastError = "创建 transport 失败";
        loggingFacade_.error("transport", "transport create failed kind=" + std::string(transport::transportKindId(kind)) +
                                              " error=" + dockStore_.commState().lastError);
        return;
    }

    loggingFacade_.info("transport", "transport open requested kind=" + std::string(transport::transportKindId(kind)));
    const bool opened = transport_->open(currentTransportConfig(kind));
    if (!opened) {
        dockStore_.commState().lastError = "打开连接失败";
        loggingFacade_.error("transport", "transport open rejected kind=" +
                                              std::string(transport::transportKindId(kind)) +
                                              " error=" + dockStore_.commState().lastError);
    } else {
        dockStore_.commState().lastError.clear();
        dockStore_.commState().reconnectRequired = false;
        loggingFacade_.info("transport", "transport open submitted kind=" +
                                              std::string(transport::transportKindId(kind)));
    }

    syncDockState();
}

void Application::closeTransport()
{
    cancelAllTxRequests("连接已关闭");
    if (activeConnection_.has_value()) {
        resetStreamBufferAlertState(activeConnection_->connectionId);
    } else {
        resetStreamBufferAlertState();
    }
    std::string recordingError;
    if (rawCaptureRecording_.isOpen() && !stopRawCaptureRecording(recordingError)) {
        loggingFacade_.error("raw_capture", "停止完整原始数据录制失败: " + recordingError);
    }
    if (transport_) {
        loggingFacade_.info("transport", "transport close requested kind=" +
                                          std::string(transport::transportKindId(dockStore_.commState().kind)));
        transport_->close();
    }
    activeConnection_.reset();
    if (responsiveBacklogMode() && runtimeConfig_.gui.realtimeBacklog.discardBacklogOnDisconnect) {
        const auto counts = clearPendingRealtimeBacklog();
        logRealtimeBacklogDiscard(counts);
    } else {
        detachPendingRealtimeBacklogFromConnection();
    }
    transport_.reset();
    syncDockState();
}

bool Application::sendManualPayload(const std::string& payload, bool hexMode)
{
    if (!transport_ || transport_->state() != transport::TransportState::Open) {
        dockStore_.commState().lastError = "连接未打开，无法发送";
        loggingFacade_.warn("transport", dockStore_.commState().lastError);
        return false;
    }

    std::vector<std::uint8_t> bytes;
    if (hexMode) {
        if (protocol_utils::countHexDigits(payload) % 2 != 0) {
            dockStore_.commState().lastError = "HEX 文本必须按完整字节输入";
            loggingFacade_.warn("transport", dockStore_.commState().lastError);
            return false;
        }
        const auto parsed = protocol_utils::hexToBytes(payload);
        if (!parsed.has_value()) {
            dockStore_.commState().lastError = "HEX 文本解析失败";
            loggingFacade_.warn("transport", dockStore_.commState().lastError);
            return false;
        }
        bytes = *parsed;
    } else {
        bytes.assign(payload.begin(), payload.end());
    }

    if (!transport_->send(bytes)) {
        dockStore_.commState().lastError = "发送失败";
        loggingFacade_.error("transport", dockStore_.commState().lastError);
        return false;
    }
    if (activeConnection_.has_value()) {
        appendTransferRow(dock::ReceiveRow{
            .timestampMs = nowMs(),
            .direction = "TX",
            .endpoint = activeConnection_->endpoint,
            .bytes = bytes,
            .message = {},
        });
    }
    dockStore_.commState().lastError.clear();
    loggingFacade_.info("transport", "手动发送成功");
    return true;
}

void Application::appendTransferRow(dock::ReceiveRow row)
{
    // 核心流程：RawChunks 模式只维护原始历史；逐帧解析延迟到 ParsedFrames 视图，避免高速 RX 时主线程逐帧膨胀。
    if (dockStore_.receiveState().displayMode == dock::TransferLogDisplayMode::ParsedFrames) {
        appendTransferFrameRows(row);
    }
    dockStore_.appendReceiveRow(std::move(row));
}

void Application::appendLiveRawCapture(const transport::TransportBytesEvent& event)
{
    auto& wave = dockStore_.waveState();
    const auto& lua = dockStore_.luaState();
    if (!wave.rawCapture.payload.empty() &&
        (wave.rawCapture.protocolDir != lua.protocolDir || wave.rawCapture.protocolName != lua.protocolName)) {
        wave.rawCapture = {};
    }
    if (wave.rawCapture.payload.empty()) {
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
    auto& rawCapture = dockStore_.waveState().rawCapture;
    rawCapture.events.push_back(event);
    if (event.type == plot::RawCaptureEventType::RxBytes) {
        rawCapture.payload.insert(rawCapture.payload.end(), event.bytes.begin(), event.bytes.end());
    }
}

bool Application::applyPlotSetup(const plot::RawCapturePlotSetupEventData& setup)
{
    auto& wave = dockStore_.waveState();
    const auto previousConfig = wave.buffer.viewConfig();
    const bool configChanged = !nearlyEqual(previousConfig.timeScale, setup.view.timeScale) ||
                               previousConfig.timeUnit != setup.view.timeUnit ||
                               !nearlyEqual(previousConfig.verticalMin, setup.view.verticalMin) ||
                               !nearlyEqual(previousConfig.verticalMax, setup.view.verticalMax) ||
                               previousConfig.verticalUnit != setup.view.verticalUnit ||
                               previousConfig.historyLimit != setup.view.historyLimit;
    const bool channelsChanged = !sameChannelSpecs(setup.channels, wave.buffer);
    const bool channelIdentityChanged = !sameChannelIdentity(setup.channels, wave.buffer);

    // 核心流程：Lua 和 psraw 回放共用同一套配置应用逻辑，确保通道默认值、覆盖状态和视图重置一致。
    if (setup.resetHistory) {
        wave.buffer.clear();
    }
    auto viewConfig = setup.view;
    viewConfig.displayFormula = wave.view.displayFormula;
    wave.buffer.setViewConfig(viewConfig);
    wave.buffer.configureChannels(setup.channels.size());
    wave.defaultChannelSpecs.clear();
    wave.defaultChannelSpecs.reserve(setup.channels.size());
    const bool shouldResetOverrides = setup.resetHistory || channelIdentityChanged;
    if (shouldResetOverrides) {
        wave.channelOverrides.clear();
    }
    wave.channelOverrides.resize(setup.channels.size());
    for (std::size_t index = 0; index < setup.channels.size(); ++index) {
        const auto defaultSpec = setup.channels[index];
        auto effectiveSpec = defaultSpec;
        if (!shouldResetOverrides && index < wave.channelOverrides.size()) {
            const auto& overrideState = wave.channelOverrides[index];
            if (overrideState.labelOverridden) {
                effectiveSpec.label = overrideState.label;
            }
            if (overrideState.ratioOverridden) {
                effectiveSpec.ratio = overrideState.ratio;
            }
            if (overrideState.scaleOverridden) {
                effectiveSpec.scale = overrideState.scale;
            }
            if (overrideState.offsetOverridden) {
                effectiveSpec.offset = overrideState.offset;
            }
            if (overrideState.bitYOffsetOverridden) {
                effectiveSpec.bitDisplay.yOffset = overrideState.bitYOffset;
            }
        }
        wave.defaultChannelSpecs.push_back(defaultSpec);
        wave.buffer.setChannelSpec(index, std::move(effectiveSpec));
    }
    const bool verticalDefaultsChanged = !nearlyEqual(previousConfig.verticalMin, setup.view.verticalMin) ||
                                         !nearlyEqual(previousConfig.verticalMax, setup.view.verticalMax);
    const bool shouldResetView = setup.resetHistory || configChanged || channelsChanged;
    if (shouldResetView) {
        wave.view.visibleDuration =
            (std::max)(wave.view.minVisibleTimeSpan, (std::max)(setup.view.timeScale * 1000.0, setup.view.timeScale));
        if (setup.resetHistory || verticalDefaultsChanged) {
            wave.view.manualVerticalMin = setup.view.verticalMin;
            wave.view.manualVerticalMax = setup.view.verticalMax;
            wave.view.viewMinValue = setup.view.verticalMin;
            wave.view.viewMaxValue = setup.view.verticalMax;
        }
        wave.view.initialized = false;
        // 核心流程：运行中的 plot.setup(reset_history=true) 只重建历史和默认视口，不覆盖用户手动暂停跟随状态。
        wave.view.defaultViewportPending = true;
        wave.statusMessage = "Lua 已更新波形通道配置";
    }
    return true;
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
    if (rawCaptureRecording_.isOpen()) {
        std::string error;
        if (!rawCaptureRecording_.appendEvent(event, error)) {
            loggingFacade_.error("raw_capture", "录制 plot.setup 事件失败: " + error);
        }
    }
}

void Application::appendRawCaptureRecording(const transport::TransportBytesEvent& event)
{
    if (!rawCaptureRecording_.isOpen() || event.bytes.empty()) {
        return;
    }

    std::string error;
    if (rawCaptureRecording_.appendEvent(
            plot::RawCaptureEvent{
                .type = plot::RawCaptureEventType::RxBytes,
                .timestampMs = event.context.timestampMs,
                .bytes = event.bytes,
                .profile = {},
                .plotSetup = {},
            },
            error)) {
        return;
    }

    const auto path = rawCaptureRecording_.path();
    std::string closeError;
    static_cast<void>(rawCaptureRecording_.close(closeError));
    const auto message = "完整原始数据录制失败: " + error + " (" + path.generic_string() + ")";
    setStatusMessage(message, true);
    loggingFacade_.error("raw_capture", message);
}

std::optional<Application::TransferFrameParserState> Application::makeTransferFrameParserState() const
{
    const auto snapshot = scriptWorker_.snapshot();
    const auto bufferDefinition = snapshot.streamBuffer;
    auto frameDefinitions = snapshot.streamFrames;
    if (!bufferDefinition.has_value() || frameDefinitions.empty()) {
        return std::nullopt;
    }
    auto rxFrames = frameDefinitions;
    auto txFrames = std::move(frameDefinitions);
    return TransferFrameParserState{
        .rx = scripting::FrameStreamParser(*bufferDefinition, std::move(rxFrames)),
        .tx = scripting::FrameStreamParser(*bufferDefinition, std::move(txFrames)),
    };
}

void Application::resetTransferFrameParser()
{
    // 核心流程：收发记录视图使用独立 parser，不复用 Lua 回调 parser，避免 UI 展示影响协议运行态。
    transferFrameParser_ = makeTransferFrameParserState();
}

void Application::resetTransferFrameDisplayState()
{
    dockStore_.clearTransferFrameRows();
    pendingTransferFrameRows_.clear();
    resetTransferFrameParser();
}

dock::ReceiveRow Application::makeTransferFrameRow(const dock::ReceiveRow& sourceRow,
                                                   const scripting::StreamParsedFrame& frame) const
{
    return dock::ReceiveRow{
        .timestampMs = sourceRow.timestampMs,
        .direction = sourceRow.direction,
        .endpoint = sourceRow.endpoint,
        .bytes = frame.raw,
        .message = transferFrameMessage(frame),
    };
}

void Application::appendTransferFrameRows(const dock::ReceiveRow& sourceRow)
{
    if (sourceRow.bytes.empty() || (sourceRow.direction != "RX" && sourceRow.direction != "TX")) {
        return;
    }
    if (!transferFrameParser_.has_value()) {
        resetTransferFrameParser();
    }
    if (!transferFrameParser_.has_value()) {
        return;
    }

    auto& parser = sourceRow.direction == "TX" ? transferFrameParser_->tx : transferFrameParser_->rx;
    const auto batch = parser.pushBytes(sourceRow.bytes);
    if (sourceRow.direction == "RX") {
        transport::ConnectionContext context;
        if (activeConnection_.has_value() && activeConnection_->endpoint == sourceRow.endpoint) {
            context = *activeConnection_;
        } else {
            context.endpoint = sourceRow.endpoint;
            context.connectionId = 0;
            context.timestampMs = sourceRow.timestampMs;
            context.readyForIo = false;
        }
        handleStreamBufferAlert(context, batch, parser.bufferDefinition());
    }
    if (batch.frames.empty()) {
        // RX 半包先留在 parser 缓冲中等待后续字节；TX 无匹配时按用户输入的原始 chunk 展示。
        if (sourceRow.direction == "TX" || !batch.errors.empty()) {
            enqueueTransferFrameRows({sourceRow});
        }
        return;
    }
    std::vector<dock::ReceiveRow> frameRows;
    frameRows.reserve(batch.frames.size());
    for (const auto& frame : batch.frames) {
        frameRows.push_back(makeTransferFrameRow(sourceRow, frame));
    }
    enqueueTransferFrameRows(std::move(frameRows));
}

void Application::rebuildTransferFrameRows()
{
    const auto rows = dockStore_.receiveState().rows;
    resetTransferFrameDisplayState();
    for (const auto& row : rows) {
        appendTransferFrameRows(row);
    }
    flushPendingTransferFrameRows(std::numeric_limits<std::size_t>::max());
}

void Application::activateParsedTransferLogView()
{
    // 核心流程：默认只解析切换后的新 raw 行；开启兼容开关时才重放旧 RawChunks 历史。
    resetTransferFrameDisplayState();
    if (!runtimeConfig_.gui.replayRawHistoryOnSchemaSwitch) {
        return;
    }
    for (const auto& row : dockStore_.receiveState().rows) {
        appendTransferFrameRows(row);
    }
    flushPendingTransferFrameRows(std::numeric_limits<std::size_t>::max());
}

void Application::applyHistoryLimits(const config::GuiLogHistoryConfig& config)
{
    dockStore_.setHistoryLimits(dock::DockHistoryLimits{
        .transferRawRows = config.transferRawLimit,
        .transferFrameRows = config.transferFrameLimit,
        .hostLogRows = config.hostLimit,
        .scriptLogRows = config.scriptLimit,
        .requestTraceRows = config.requestTraceLimit,
    });
    trimPendingTransferFrameRowsToLimit();
}

void Application::updateControlValue(const std::string& id, const scripting::ControlValue& value)
{
    transport::ConnectionContext context;
    if (activeConnection_.has_value()) {
        context = *activeConnection_;
    } else {
        // 核心流程：动态控件也可能只驱动 Lua 本地演示逻辑，未连接时仍允许回调脚本。
        context.endpoint = "detached";
        context.connectionId = 0;
        context.timestampMs = nowMs();
        context.readyForIo = false;
    }
    scriptWorker_.postControl(context, id, value);
    scriptWorker_.waitIdle();
    flushScriptOutputs();
    syncDockState();
}

bool Application::requestOscilloscopeToggle(bool currentRunning, bool targetRunning)
{
    transport::ConnectionContext context;
    if (activeConnection_.has_value()) {
        context = *activeConnection_;
    } else {
        // 核心流程：示波器启停由 Lua 决定，未连接时仍允许脚本基于 detached 上下文处理本地动作。
        context.endpoint = "detached";
        context.connectionId = 0;
        context.timestampMs = nowMs();
        context.readyForIo = false;
    }

    try {
        const bool accepted = scriptWorker_.requestOscilloscopeToggle(context, currentRunning, targetRunning);
        flushScriptOutputs();
        syncDockState();
        return accepted;
    } catch (const std::exception& ex) {
        setStatusMessage(std::string("示波器切换请求失败: ") + ex.what(), false);
    } catch (...) {
        setStatusMessage("示波器切换请求失败: 未知异常", false);
    }
    flushScriptOutputs();
    syncDockState();
    return false;
}

bool Application::restoreControlValue(const std::string& id, const scripting::ControlValue& value)
{
    if (!scriptWorker_.setControlValue(id, value)) {
        return false;
    }
    syncDockState();
    return true;
}

void Application::markCommConfigEdited(bool reconnectRequired)
{
    dockStore_.commState().reconnectRequired = reconnectRequired;
    dockStore_.markDirty(reconnectRequired ? "通讯配置已修改，需重新连接" : "通讯配置已修改");
}

void Application::markProtocolEdited()
{
    dockStore_.markDirty("协议配置已修改");
}

void Application::setStatusMessage(std::string message, bool markDirty)
{
    if (markDirty) {
        dockStore_.markDirty(message);
        return;
    }
    dockStore_.configState().statusMessage = std::move(message);
}

bool Application::setSendHexMode(bool enabled)
{
    auto& send = dockStore_.sendState();
    if (send.hexMode == enabled) {
        return true;
    }

    if (enabled) {
        const std::vector<std::uint8_t> bytes(send.payload.begin(), send.payload.end());
        send.payload = protocol_utils::bytesToHex(bytes, true);
        send.hexMode = true;
        setStatusMessage("发送框已切换到 HEX 模式");
        return true;
    }

    if (protocol_utils::countHexDigits(send.payload) % 2 != 0) {
        dockStore_.commState().lastError = "HEX 文本必须按完整字节输入，无法切回文本模式";
        loggingFacade_.warn("transport", dockStore_.commState().lastError);
        return false;
    }

    const auto parsed = protocol_utils::hexToBytes(send.payload);
    if (!parsed.has_value()) {
        dockStore_.commState().lastError = "HEX 文本解析失败，无法切回文本模式";
        loggingFacade_.warn("transport", dockStore_.commState().lastError);
        return false;
    }

    send.payload.assign(parsed->begin(), parsed->end());
    send.hexMode = false;
    setStatusMessage("发送框已切换到文本模式");
    return true;
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

bool Application::importWaveCsvData(const plot::WaveCsvData& data, std::string& error)
{
    if (data.channels.empty()) {
        error = "波形 CSV 未包含任何通道";
        return false;
    }

    auto& wave = dockStore_.waveState();
    wave.buffer.clear();
    wave.buffer.setHistoryTrimSuspended(true);
    wave.buffer.configureChannels(data.channels.size());
    wave.defaultChannelSpecs.clear();
    wave.defaultChannelSpecs.reserve(data.channels.size());
    std::size_t maxSampleCount = 0;
    for (std::size_t channelIndex = 0; channelIndex < data.channels.size(); ++channelIndex) {
        const auto& csvChannel = data.channels[channelIndex];
        plot::ChannelSpec spec{
            .label = csvChannel.label.empty() ? "CH" + std::to_string(channelIndex + 1) : csvChannel.label,
            .unit = csvChannel.unit,
        };
        wave.buffer.setChannelSpec(channelIndex, spec);
        wave.defaultChannelSpecs.push_back(spec);
        if (!csvChannel.samples.empty()) {
            // 核心流程：CSV time 是外部数据的显示时间，直接作为脚本时间轴写入，不再按采样率重解释。
            wave.buffer.append(channelIndex,
                               plot::WaveAppendRequest{
                                   .source = "csv_wave",
                                   .samples = csvChannel.samples,
                               });
            maxSampleCount = (std::max)(maxSampleCount, csvChannel.samples.size());
        }
    }
    wave.buffer.setHistoryTrimSuspended(false);
    wave.buffer.preserveHistoryLimitAtLeast(maxSampleCount);
    wave.rawCapture = {};
    wave.analysisMarkers.clear();
    wave.channelSummaries.clear();
    wave.hiddenChannelIndices.clear();
    wave.view.sampleFrequencyHz = 0.0;
    wave.view.sampleFrequencyInput.clear();
    wave.view.timeAxisSource = plot::WaveTimeAxisSource::ScriptTime;
    wave.view.defaultViewportPending = true;
    wave.view.autoFollowLatest = false;
    wave.statusMessage = "波形 CSV 已导入";
    syncDockState();
    return true;
}

bool Application::exportWaveCsv(const std::filesystem::path& path,
                                plot::WaveCsvShape shape,
                                const plot::CsvExportRange& range,
                                std::string& error) const
{
    const auto& wave = dockStore_.waveState();
    auto snapshot = wave.buffer.snapshot(-std::numeric_limits<double>::infinity(),
                                         std::numeric_limits<double>::infinity(),
                                         false);
    const auto displayData = plot::buildDisplayData(snapshot, wave.view.sampleFrequencyHz);

    plot::WaveCsvData data;
    data.shape = shape;
    data.sampleFrequencyHz = wave.view.sampleFrequencyHz;
    data.view = wave.buffer.viewConfig();
    data.channels.reserve(snapshot.channels.size());
    for (std::size_t channelIndex = 0; channelIndex < snapshot.channels.size(); ++channelIndex) {
        plot::WaveCsvChannel channel{
            .label = snapshot.channels[channelIndex].label,
            .unit = snapshot.channels[channelIndex].unit,
        };
        if (channelIndex < displayData.channels.size()) {
            channel.samples = displayData.channels[channelIndex].samples;
        }
        data.channels.push_back(std::move(channel));
    }
    return plot::writeWaveCsvFile(path, data, shape, range, error);
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
    loggingFacade_.info("session", "session import requested kind=session endpoint=" + path.generic_string());
    const auto previousConfig = runtimeConfig_;
    const auto previousWave = dockStore_.waveState();
    const auto previousCaptureProtocolOverride = captureProtocolConfigOverride_;
    auto rollbackImport = [&]() {
        std::string rollbackError;
        if (!applyConfig(previousConfig)) {
            rollbackError = "恢复导入前配置失败";
        }
        dockStore_.waveState() = previousWave;
        captureProtocolConfigOverride_ = previousCaptureProtocolOverride;
        if (!rollbackError.empty()) {
            error += "; " + rollbackError;
        }
    };

    const auto package = session::readSessionPackage(path, error);
    if (!package.has_value()) {
        return false;
    }
    if (!validateSessionPackageEntries(*package, error)) {
        return false;
    }

    const auto* configEntry = session::findSessionPackageEntry(*package, "config.yaml");
    if (configEntry == nullptr) {
        error = "现场包缺少 config.yaml";
        return false;
    }

    auto loaded = configStore_.loadText(stringFromBytes(configEntry->bytes));
    if (!loaded.error.empty()) {
        error = loaded.error;
        return false;
    }
    loaded.config.configPath = runtimeConfig_.configPath;

    std::optional<config::ProtocolConfig> persistentProtocolConfig;
    std::optional<std::string> importedProtocolDir;
    const auto protocolEntries = collectSessionProtocolEntries(*package);
    if (!protocolEntries.empty()) {
        persistentProtocolConfig = loaded.config.protocol;
        const auto protocolDir =
            std::filesystem::temp_directory_path() / ("ProtoScope-session-protocol-" + std::to_string(nowUs()));
        if (!releaseSessionProtocolEntries(protocolEntries, protocolDir, error)) {
            return false;
        }
        if (!validateReleasedProtocolDirectory(protocolDir, loaded.config.scripting.fileIo, error)) {
            return false;
        }
        loaded.config.protocol.rootDir = protocolDir.parent_path().generic_string();
        loaded.config.protocol.selectedDir = protocolDir.generic_string();
        importedProtocolDir = loaded.config.protocol.selectedDir;
    }

    std::optional<plot::RawCaptureFileData> rawCapture;
    if (!decodeSessionRawCaptureEntry(*package, importedProtocolDir, rawCapture, error)) {
        return false;
    }

    std::vector<plot::WaveAnalysisMarker> importedMarkers;
    if (!decodeSessionAnalysisMarkers(*package, importedMarkers, error)) {
        return false;
    }

    if (!applyConfig(loaded.config)) {
        error = "应用现场包配置失败";
        rollbackImport();
        return false;
    }
    if (persistentProtocolConfig.has_value()) {
        captureProtocolConfigOverride_ = *persistentProtocolConfig;
    }

    if (rawCapture.has_value()) {
        if (!importWaveRawCapture(*rawCapture, error)) {
            rollbackImport();
            return false;
        }
    }

    auto& wave = dockStore_.waveState();
    wave.analysisMarkers.clear();
    wave.analysisMarkers = std::move(importedMarkers);

    setStatusMessage("现场包已导入: " + path.generic_string());
    loggingFacade_.info("session", "session imported kind=session endpoint=" + path.generic_string());
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
    if (metadata.protocolName.empty() || metadata.protocolDir.empty()) {
        error = "当前协议元数据不完整，无法开始录制";
        return false;
    }

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

bool Application::validateRawCaptureImport(const plot::RawCaptureFileData& capture, std::string& error) const
{
    const auto& lua = dockStore_.luaState();
    if (!lua.loaded) {
        error = "当前协议尚未加载";
        return false;
    }
    if (capture.protocolDir.empty() || capture.protocolDir != lua.protocolDir) {
        error = "导入文件协议目录与当前工作区不一致";
        return false;
    }
    return true;
}

void Application::prepareRawCaptureImportReplay(const plot::RawCaptureFileData& capture)
{
    // 核心流程：导入回放必须先清空旧波形与旧原始缓冲，再走一次 on_bytes -> flushScriptPlots，
    // 避免导入样本与现场采集样本混在同一份波形/原始容器里。
    scriptWorker_.waitIdle();
    static_cast<void>(scriptWorker_.drainOutputs());
    const auto discarded = clearPendingRealtimeBacklog();
    logRealtimeBacklogDiscard(discarded);
    resetWaveHistory();
    auto& wave = dockStore_.waveState();
    wave.rawCapture = capture;
    wave.view.sampleFrequencyHz = capture.sampleFrequencyHz;
    wave.view.sampleFrequencyInput = formatFrequencyInput(capture.sampleFrequencyHz);
    wave.view.sampleFrequencyError.clear();
    wave.buffer.setHistoryTrimSuspended(true);
    loggingFacade_.info("raw_capture",
                        "raw import prepared kind=raw_import endpoint=" + capture.protocolDir +
                            " bytes=" + std::to_string(capture.payload.size()) +
                            " events=" + std::to_string(capture.events.size()));
}

transport::ConnectionContext Application::makeRawCaptureReplayContext(const plot::RawCaptureFileData& capture) const
{
    transport::ConnectionContext replayContext;
    replayContext.endpoint = "psraw-import";
    replayContext.connectionId = 0;
    replayContext.timestampMs = capture.capturedAtMs == 0 ? nowMs() : capture.capturedAtMs;
    replayContext.readyForIo = false;
    return replayContext;
}

bool Application::replayRawCaptureEvents(const plot::RawCaptureFileData& capture, std::string& error)
{
    loggingFacade_.info("raw_capture",
                        "raw replay events kind=raw_replay endpoint=" + capture.protocolDir +
                            " events=" + std::to_string(capture.events.size()));
    auto replayContext = makeRawCaptureReplayContext(capture);
    suppressRawCaptureProfileEvents_ = true;
    suppressRawCapturePlotSetupEvents_ = true;

    for (const auto& recordedEvent : capture.events) {
        replayContext.timestampMs =
            recordedEvent.timestampMs == 0 ? replayContext.timestampMs : recordedEvent.timestampMs;
        if (!replayRawCaptureEvent(recordedEvent, replayContext, error)) {
            cancelRawCaptureImportReplay();
            return false;
        }
    }

    flushScriptOutputs();
    suppressRawCaptureProfileEvents_ = false;
    suppressRawCapturePlotSetupEvents_ = false;
    return true;
}

bool Application::pumpRawCaptureReplay(std::string& error)
{
    if (!rawCaptureReplay_.loaded || !rawCaptureReplay_.playing) {
        return false;
    }
    if (rawCaptureReplay_.capture.events.empty()) {
        loggingFacade_.trace("raw_capture",
                             "raw replay payload kind=raw_replay endpoint=" +
                                 rawCaptureReplay_.capture.protocolDir +
                                 " bytes=" + std::to_string(rawCaptureReplay_.capture.payload.size()));
        replayRawCaptureBytes(rawCaptureReplay_.context, rawCaptureReplay_.capture.payload);
        finishRawCaptureImportReplay();
        rawCaptureReplay_.playing = false;
        rawCaptureReplay_.eventIndex = rawCaptureReplay_.capture.payload.empty() ? 0U : 1U;
        rawCaptureReplay_.lastPumpMs = 0;
        rawCaptureReplay_.accumulatedMs = 0.0;
        return true;
    }

    const auto now = nowMs();
    if (rawCaptureReplay_.lastPumpMs == 0) {
        rawCaptureReplay_.lastPumpMs = now;
    }
    rawCaptureReplay_.accumulatedMs +=
        static_cast<double>(now - rawCaptureReplay_.lastPumpMs) * rawCaptureReplay_.speed;
    rawCaptureReplay_.lastPumpMs = now;

    bool changed = false;
    while (rawCaptureReplay_.eventIndex < rawCaptureReplay_.capture.events.size()) {
        double waitMs = 0.0;
        if (rawCaptureReplay_.eventIndex == 0) {
            const auto& current = rawCaptureReplay_.capture.events[0];
            if (current.timestampMs > rawCaptureReplay_.capture.capturedAtMs) {
                waitMs = static_cast<double>(current.timestampMs - rawCaptureReplay_.capture.capturedAtMs);
            }
        } else {
            const auto& previous = rawCaptureReplay_.capture.events[rawCaptureReplay_.eventIndex - 1];
            const auto& current = rawCaptureReplay_.capture.events[rawCaptureReplay_.eventIndex];
            if (current.timestampMs > previous.timestampMs) {
                waitMs = static_cast<double>(current.timestampMs - previous.timestampMs);
            }
        }
        if (waitMs > rawCaptureReplay_.accumulatedMs) {
            break;
        }
        rawCaptureReplay_.accumulatedMs -= waitMs;
        if (!replayRawCaptureEventAt(rawCaptureReplay_.eventIndex, error)) {
            rawCaptureReplay_.playing = false;
            cancelRawCaptureImportReplay();
            rawCaptureReplay_ = RawCaptureReplayState{};
            dockStore_.waveState().statusMessage = "原始回放时间轴已卸载: " + error;
            return false;
        }
        changed = true;
    }

    if (rawCaptureReplay_.eventIndex >= rawCaptureReplay_.capture.events.size()) {
        finishRawCaptureImportReplay();
        rawCaptureReplay_.playing = false;
        rawCaptureReplay_.lastPumpMs = 0;
        rawCaptureReplay_.accumulatedMs = 0.0;
        return true;
    }
    return changed;
}

bool Application::replayRawCaptureEventAt(const std::size_t eventIndex, std::string& error)
{
    if (!rawCaptureReplay_.loaded) {
        error = "尚未载入原始回放时间轴";
        return false;
    }
    if (eventIndex >= rawCaptureReplay_.capture.events.size()) {
        return true;
    }

    const auto& event = rawCaptureReplay_.capture.events[eventIndex];
    rawCaptureReplay_.context.timestampMs =
        event.timestampMs == 0 ? rawCaptureReplay_.context.timestampMs : event.timestampMs;
    suppressRawCaptureProfileEvents_ = true;
    suppressRawCapturePlotSetupEvents_ = true;
    if (!replayRawCaptureEvent(event, rawCaptureReplay_.context, error)) {
        suppressRawCaptureProfileEvents_ = false;
        suppressRawCapturePlotSetupEvents_ = false;
        return false;
    }
    suppressRawCaptureProfileEvents_ = false;
    suppressRawCapturePlotSetupEvents_ = false;
    flushScriptOutputs();
    rawCaptureReplay_.eventIndex = eventIndex + 1;
    return true;
}

bool Application::replayRawCaptureEvent(const plot::RawCaptureEvent& event,
                                        transport::ConnectionContext& replayContext,
                                        std::string& error)
{
    if (event.type == plot::RawCaptureEventType::ProfileSet) {
        return applyRawCaptureRuntimeProfileEvent(event, false, error);
    }
    if (event.type == plot::RawCaptureEventType::ProfileClear) {
        return applyRawCaptureRuntimeProfileEvent(event, true, error);
    }
    if (event.type == plot::RawCaptureEventType::PlotSetup) {
        auto& wave = dockStore_.waveState();
        // 核心流程：raw 回放可能先由 Lua on_open/on_bytes 产生同一 setup；完全一致时跳过，避免 reset_history
        // 二次清空样本。
        if (!sameAppliedPlotSetup(wave, event.plotSetup)) {
            applyPlotSetup(event.plotSetup);
        }
        return true;
    }
    if (!event.bytes.empty()) {
        replayRawCaptureBytes(replayContext, event.bytes);
    }
    return true;
}

bool Application::applyRawCaptureRuntimeProfileEvent(const plot::RawCaptureEvent& event,
                                                     bool cleared,
                                                     std::string& error)
{
    std::string profileError;
    if (!scriptWorker_.applyStreamRuntimeProfileEvent(
            scripting::StreamRuntimeProfileEvent{
                .cleared = cleared,
                .frameName = event.profile.frameName,
                .length = cleared ? 0 : event.profile.length,
                .channelMap = cleared ? std::vector<std::size_t>{} : event.profile.channelMap,
            },
            profileError)) {
        error = cleared ? "导入 profile_clear 失败: " + profileError : "导入 profile_set 失败: " + profileError;
        return false;
    }

    scriptWorker_.waitIdle();
    flushScriptOutputs();
    return true;
}

void Application::replayRawCaptureBytes(const transport::ConnectionContext& replayContext,
                                        const std::vector<std::uint8_t>& bytes)
{
    loggingFacade_.trace("raw_capture",
                         "raw replay bytes" + transportContextFields("raw_replay", replayContext) +
                             " bytes=" + std::to_string(bytes.size()) +
                             payloadPreviewSuffix(loggingFacade_.currentConfig(), bytes));
    std::size_t cursor = 0;
    while (cursor < bytes.size()) {
        const auto chunkSize = (std::min)(kRawCaptureReplayChunkBytes, bytes.size() - cursor);
        std::vector<std::uint8_t> chunk(bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                                        bytes.begin() + static_cast<std::ptrdiff_t>(cursor + chunkSize));
        scriptWorker_.postTransportBytes(transport::TransportBytesEvent{replayContext, std::move(chunk)});
        if (runtimeConfig_.scripting.workerEnabled) {
            // 核心流程：异步 worker 需要先等到脚本消费完当前 chunk，再把输出合并到回放结果。
            scriptWorker_.waitIdle();
        }
        flushScriptOutputs();
        cursor += chunkSize;
    }
}

void Application::finishRawCaptureImportReplay()
{
    auto& wave = dockStore_.waveState();
    flushScriptOutputs();
    const auto importedSnapshot =
        wave.buffer.snapshot(-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), false);
    std::size_t importedHistoryLimit = 0;
    for (const auto& channel : importedSnapshot.channels) {
        importedHistoryLimit = (std::max)(importedHistoryLimit, channel.totalSamples);
    }
    wave.buffer.setHistoryTrimSuspended(false);
    wave.buffer.preserveHistoryLimitAtLeast(importedHistoryLimit);
    syncDockState();
    wave.statusMessage = "原始波形已导入";
    loggingFacade_.info("raw_capture", "raw import finished kind=raw_import endpoint=" +
                                           dockStore_.luaState().protocolDir);
}

void Application::cancelRawCaptureImportReplay()
{
    suppressRawCaptureProfileEvents_ = false;
    suppressRawCapturePlotSetupEvents_ = false;
    dockStore_.waveState().buffer.setHistoryTrimSuspended(false);
}

bool Application::importWaveRawCapture(const plot::RawCaptureFileData& capture, std::string& error)
{
    if (!validateRawCaptureImport(capture, error)) {
        return false;
    }

    prepareRawCaptureImportReplay(capture);
    if (!capture.events.empty()) {
        if (!replayRawCaptureEvents(capture, error)) {
            return false;
        }
    } else if (!capture.payload.empty()) {
        replayRawCaptureBytes(makeRawCaptureReplayContext(capture), capture.payload);
    }

    finishRawCaptureImportReplay();
    return true;
}

bool Application::loadRawCaptureReplayTimeline(const plot::RawCaptureFileData& capture, std::string& error)
{
    if (!validateRawCaptureImport(capture, error)) {
        return false;
    }
    if (capture.events.empty() && capture.payload.empty()) {
        error = "原始波形没有可回放事件";
        return false;
    }

    if (rawCaptureReplay_.loaded) {
        cancelRawCaptureImportReplay();
    }
    auto replayCapture = capture;
    if (replayCapture.events.empty() && !replayCapture.payload.empty()) {
        replayCapture.events.push_back(plot::RawCaptureEvent{
            .type = plot::RawCaptureEventType::RxBytes,
            .timestampMs = replayCapture.capturedAtMs,
            .bytes = replayCapture.payload,
            .profile = {},
            .plotSetup = {},
        });
    }
    prepareRawCaptureImportReplay(replayCapture);
    rawCaptureReplay_ = RawCaptureReplayState{};
    rawCaptureReplay_.loaded = true;
    rawCaptureReplay_.playing = false;
    rawCaptureReplay_.capture = std::move(replayCapture);
    rawCaptureReplay_.context = makeRawCaptureReplayContext(rawCaptureReplay_.capture);
    rawCaptureReplay_.speed = 1.0;
    dockStore_.waveState().statusMessage = "原始回放时间轴已载入";
    return true;
}

void Application::unloadRawCaptureReplayTimeline()
{
    if (!rawCaptureReplay_.loaded) {
        return;
    }
    cancelRawCaptureImportReplay();
    rawCaptureReplay_ = RawCaptureReplayState{};
    dockStore_.waveState().statusMessage = "原始回放时间轴已卸载";
}

bool Application::playRawCaptureReplay(std::string& error)
{
    if (!rawCaptureReplay_.loaded) {
        error = "尚未载入原始回放时间轴";
        return false;
    }
    rawCaptureReplay_.playing = true;
    rawCaptureReplay_.lastPumpMs = nowMs();
    return true;
}

void Application::pauseRawCaptureReplay()
{
    rawCaptureReplay_.playing = false;
}

bool Application::stepRawCaptureReplay(std::string& error)
{
    if (!rawCaptureReplay_.loaded) {
        error = "尚未载入原始回放时间轴";
        return false;
    }
    rawCaptureReplay_.playing = false;
    return replayRawCaptureEventAt(rawCaptureReplay_.eventIndex, error);
}

bool Application::seekRawCaptureReplay(const std::size_t eventIndex, std::string& error)
{
    if (!rawCaptureReplay_.loaded) {
        error = "尚未载入原始回放时间轴";
        return false;
    }

    const auto capture = rawCaptureReplay_.capture;
    const auto speed = rawCaptureReplay_.speed;
    const bool wasPlaying = rawCaptureReplay_.playing;
    const auto targetIndex = (std::min)(eventIndex, capture.events.empty() ? std::size_t{1} : capture.events.size());
    if (targetIndex >= kRawCaptureReplaySeekNoticeEvents) {
        dockStore_.waveState().statusMessage = "正在从头重放原始回放时间轴以完成定位...";
    }
    prepareRawCaptureImportReplay(capture);
    rawCaptureReplay_ = RawCaptureReplayState{};
    rawCaptureReplay_.loaded = true;
    rawCaptureReplay_.playing = false;
    rawCaptureReplay_.capture = capture;
    rawCaptureReplay_.context = makeRawCaptureReplayContext(capture);
    rawCaptureReplay_.speed = speed;
    for (std::size_t index = 0; index < targetIndex; ++index) {
        if (!replayRawCaptureEventAt(index, error)) {
            cancelRawCaptureImportReplay();
            rawCaptureReplay_ = RawCaptureReplayState{};
            dockStore_.waveState().statusMessage = "原始回放定位失败，时间轴已卸载";
            return false;
        }
    }
    if (rawCaptureReplay_.eventIndex >= rawCaptureReplay_.capture.events.size()) {
        finishRawCaptureImportReplay();
        rawCaptureReplay_.playing = false;
    } else {
        rawCaptureReplay_.playing = wasPlaying;
        rawCaptureReplay_.lastPumpMs = nowMs();
    }
    if (targetIndex >= kRawCaptureReplaySeekNoticeEvents) {
        dockStore_.waveState().statusMessage = "原始回放定位完成";
    }
    return true;
}

void Application::setRawCaptureReplaySpeed(const double speed)
{
    rawCaptureReplay_.speed = (std::max)(0.1, (std::min)(speed, 16.0));
}

Application::RawCaptureReplayStatus Application::rawCaptureReplayStatus() const
{
    const auto eventCount = rawCaptureReplay_.capture.events.empty()
                                ? (rawCaptureReplay_.capture.payload.empty() ? 0U : 1U)
                                : rawCaptureReplay_.capture.events.size();
    const double progress =
        eventCount == 0U
            ? 0.0
            : (std::min)(1.0, static_cast<double>(rawCaptureReplay_.eventIndex) / static_cast<double>(eventCount));
    return RawCaptureReplayStatus{
        .loaded = rawCaptureReplay_.loaded,
        .playing = rawCaptureReplay_.playing,
        .eventIndex = rawCaptureReplay_.eventIndex,
        .eventCount = eventCount,
        .progress = progress,
        .speed = rawCaptureReplay_.speed,
    };
}

bool Application::loadElfStaticAddressFile(const std::filesystem::path& path, std::string& error)
{
    if (!elfStaticView_.loadFile(path, error)) {
        loggingFacade_.warn("elf", "elf load failed kind=elf endpoint=" + path.generic_string() + " error=" + error);
        return false;
    }
    ++elfStaticAddressRevision_;
    setStatusMessage("ELF/ElfStaticView 数据文件已加载: " + elfStaticView_.sourcePath());
    loggingFacade_.info("elf", "elf loaded kind=elf endpoint=" + elfStaticView_.sourcePath());
    return true;
}

void Application::clearElfStaticAddressFile()
{
    const bool hadLoadedContext = elfStaticView_.loaded() || !elfStaticView_.sourcePath().empty();
    elfStaticView_.clear();
    if (hadLoadedContext) {
        ++elfStaticAddressRevision_;
        loggingFacade_.info("elf", "elf cleared kind=elf");
    }
}

std::uint64_t Application::elfStaticAddressRevision() const
{
    return elfStaticAddressRevision_;
}

std::vector<scripting::ElfSymbolValue> Application::queryElfStaticAddresses(const std::string& queryText,
                                                                            std::size_t limit) const
{
    std::vector<scripting::ElfSymbolValue> symbols;
    for (const auto& entry : elfStaticView_.query(queryText, limit)) {
        symbols.push_back(scripting::ElfSymbolValue{
            .label = entry.label,
            .value = entry.value,
            .type = entry.type,
        });
    }
    return symbols;
}

void Application::refreshSelectedElfSymbolControls()
{
    const auto comboConfig = runtimeConfig_.gui.elfSymbolCombo;
    if (!comboConfig.autoRefreshSelectedAddress) {
        return;
    }

    const auto luaSnapshot = scriptWorker_.snapshot();
    for (const auto& control : luaSnapshot.controlStates) {
        if (control.descriptor.type != scripting::ControlType::ElfSymbolCombo) {
            continue;
        }
        const auto* current = std::get_if<scripting::ElfSymbolValue>(&control.value);
        if (current == nullptr || current->label.empty()) {
            continue;
        }

        const auto refreshed = elfStaticView_.findExactLabel(current->label);
        if (!refreshed.has_value() || (refreshed->value == current->value && refreshed->type == current->type)) {
            continue;
        }

        const scripting::ElfSymbolValue refreshedValue{
            .label = refreshed->label,
            .value = refreshed->value,
            .type = refreshed->type,
        };
        // 核心流程：默认只同步当前控件值；显式配置后才复用 updateControlValue 触发 Lua on_control。
        if (comboConfig.autoRefreshEmitOnControl) {
            updateControlValue(control.descriptor.id, refreshedValue);
        } else {
            static_cast<void>(restoreControlValue(control.descriptor.id, refreshedValue));
        }
    }
    flushScriptOutputs();
    syncDockState();
}

void Application::resetWaveHistory()
{
    auto& wave = dockStore_.waveState();
    wave.buffer.clear();
    wave.rawCapture = {};
    wave.analysisMarkers.clear();
    wave.channelSummaries.clear();
    wave.view.initialized = false;
    wave.view.centerTime = 0.0;
    wave.view.viewMinTime = 0.0;
    wave.view.viewMaxTime = wave.view.visibleDuration;
    wave.view.viewMinValue = wave.view.manualVerticalMin;
    wave.view.viewMaxValue = wave.view.manualVerticalMax;
    wave.view.autoFollowLatest = true;
    wave.view.defaultViewportPending = true;
    wave.statusMessage = "波形历史已清空";
}

bool Application::exportWaveAnalysisReport(const std::filesystem::path& path, std::string& error) const
{
    const auto& wave = dockStore_.waveState();
    try {
        if (path.has_parent_path()) {
            std::error_code directoryError;
            std::filesystem::create_directories(path.parent_path(), directoryError);
            if (directoryError) {
                error = "创建分析报告目录失败: " + directoryError.message();
                return false;
            }
        }

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.good()) {
            error = "无法写入波形分析报告";
            return false;
        }

        const auto& view = wave.view;
        out << "section,key,value\n";
        out << "summary,protocol," << csvEscape(dockStore_.luaState().protocolName) << '\n';
        out << "summary,protocol_dir," << csvEscape(dockStore_.luaState().protocolDir) << '\n';
        out << "summary,sample_frequency_hz," << view.sampleFrequencyHz << '\n';
        out << "summary,view_min_time," << view.viewMinTime << '\n';
        out << "summary,view_max_time," << view.viewMaxTime << '\n';
        out << "summary,raw_capture_bytes," << wave.rawCapture.payload.size() << '\n';
        out << "summary,raw_capture_events," << wave.rawCapture.events.size() << '\n';
        out << "summary,markers," << wave.analysisMarkers.size() << '\n';
        out << '\n';
        out << "marker,id,label,note,channel_index,start_time,end_time\n";
        for (const auto& marker : wave.analysisMarkers) {
            out << "marker," << marker.id << ',' << csvEscape(marker.label) << ',' << csvEscape(marker.note) << ','
                << marker.channelIndex << ',' << marker.startTime << ',' << marker.endTime << '\n';
        }
        if (!out.good()) {
            error = "写入波形分析报告失败";
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

std::optional<std::uint64_t> Application::nextWakeupAtMs() const
{
    const auto scriptSnapshot = scriptWorker_.snapshot();
    const auto pendingWorkerRxBytes = scriptWorker_.pendingRxBytes();
    if (rawCaptureReplay_.loaded && rawCaptureReplay_.playing) {
        return nowMs();
    }
    if (!pendingTransportEvents_.empty() || !pendingRxByteChunks_.empty() || !pendingTransferFrameRows_.empty() ||
        scriptSnapshot.pendingPlotAppends > 0U || pendingWorkerRxBytes > 0U) {
        return nowMs();
    }
    auto nextWakeup = scriptSnapshot.nextWakeupAtMs;
    if (activeHalfDuplexRequest_.has_value()) {
        if (!nextWakeup.has_value() || activeHalfDuplexRequest_->waitDeadlineMs < *nextWakeup) {
            nextWakeup = activeHalfDuplexRequest_->waitDeadlineMs;
        }
    }
    return nextWakeup;
}

void Application::setTransportFactoryForTest(
    std::function<std::unique_ptr<transport::ITransport>(transport::TransportKind)> factory)
{
    transportFactoryForTest_ = std::move(factory);
}

std::unique_ptr<transport::ITransport> Application::createTransport(transport::TransportKind kind) const
{
    if (transportFactoryForTest_) {
        return transportFactoryForTest_(kind);
    }
    return transport::createTransport(kind);
}

transport::TransportConfig Application::currentTransportConfig(transport::TransportKind kind) const
{
    const auto& comm = dockStore_.commState();
    switch (kind) {
        case transport::TransportKind::TcpClient:
            return [&] {
                auto config = comm.tcpClient;
                config.readBufferBytes = runtimeConfig_.receive.transportReadBufferBytes;
                return config;
            }();
        case transport::TransportKind::TcpServer:
            return [&] {
                auto config = comm.tcpServer;
                config.readBufferBytes = runtimeConfig_.receive.transportReadBufferBytes;
                return config;
            }();
        case transport::TransportKind::Serial:
            return [&] {
                auto config = comm.serial;
                config.readBufferBytes = runtimeConfig_.receive.transportReadBufferBytes;
                return config;
            }();
        case transport::TransportKind::UdpPeer:
            return [&] {
                auto config = comm.udpPeer;
                config.readBufferBytes = runtimeConfig_.receive.transportReadBufferBytes;
                return config;
            }();
    }
    auto config = comm.tcpClient;
    config.readBufferBytes = runtimeConfig_.receive.transportReadBufferBytes;
    return config;
}

void Application::syncDockState()
{
    auto& comm = dockStore_.commState();
    if (transport_) {
        comm.state = transport_->state();
        comm.txCount = transport_->txCount();
        comm.rxCount = transport_->rxCount();
    } else {
        comm.state = transport::TransportState::Closed;
    }
    const auto scriptSnapshot = scriptWorker_.snapshot();
    const auto pendingWorkerRxBytes = scriptWorker_.pendingRxBytes();
    comm.pendingRxBytes = pendingRxByteCount() + pendingWorkerRxBytes;
    comm.pendingTransferFrameRows = pendingTransferFrameRows_.size();
    comm.pendingPlotAppends = scriptSnapshot.pendingPlotAppends + pendingScriptPlotAppends_.size();
    comm.rxInputQueueBytes = pendingRxByteCount();
    comm.parserPendingBytes = pendingWorkerRxBytes;
    comm.postprocessPendingBatches = 0U;
    comm.luaPendingItems = scriptSnapshot.inputQueueSize;
    comm.uiPendingItems =
        scriptSnapshot.outputQueueSize + pendingTransferFrameRows_.size() + pendingScriptPlotAppends_.size();
    comm.postprocessWorkerThreads = scriptSnapshot.postprocessWorkerThreads;
    comm.backlogWarning.clear();
    const auto rawFirstWarnBytes = runtimeConfig_.gui.realtimeBacklog.rawFirstBacklogWarnBytes;
    if (rawFirstWarnBytes > 0U && comm.pendingRxBytes >= rawFirstWarnBytes) {
        comm.backlogWarning = "raw-first backlog 已超过告警阈值；原始数据继续保留，派生 UI 可能延后或降级";
    } else if (runtimeConfig_.gui.realtimeBacklog.derivedBacklogDegradeEnabled &&
               (comm.pendingTransferFrameRows > runtimeConfig_.gui.realtimeBacklog.transferFrameRowsPerPump ||
                comm.pendingPlotAppends > runtimeConfig_.gui.realtimeBacklog.plotAppendsPerPump)) {
        comm.backlogWarning = "派生 UI backlog 已超过单轮预算；transfer rows / plot append 将分批追赶";
    }
    comm.lastErrorSummary = scriptSnapshot.lastTransportStats.lastErrorSummary;
    maybeLogCommPressureDebug(comm);

    auto& lua = dockStore_.luaState();
    lua.docks = scriptSnapshot.docks;
    lua.controls = scriptSnapshot.controls;
    lua.controlStates = scriptSnapshot.controlStates;

    auto& wave = dockStore_.waveState();
    const auto waveRevision = wave.buffer.dataRevision();
    if (!cachedWaveSummaryRevision_.has_value() || *cachedWaveSummaryRevision_ != waveRevision) {
        wave.channelSummaries.clear();
        const auto snapshot =
            wave.buffer.snapshot(-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity());
        for (const auto& channel : snapshot.channels) {
            wave.channelSummaries.push_back(channel.label + " samples=" + std::to_string(channel.totalSamples));
        }
        cachedWaveSummaryRevision_ = waveRevision;
    }
}

void Application::maybeLogCommPressureDebug(const dock::CommDockState& comm)
{
    if (loggingFacade_.currentConfig().level != config::LogLevel::Debug) {
        return;
    }

    CommPressureDebugSnapshot snapshot{
        .pendingRxBytes = comm.pendingRxBytes,
        .pendingTransferFrameRows = comm.pendingTransferFrameRows,
        .pendingPlotAppends = comm.pendingPlotAppends,
        .rxInputQueueBytes = comm.rxInputQueueBytes,
        .parserPendingBytes = comm.parserPendingBytes,
        .postprocessPendingBatches = comm.postprocessPendingBatches,
        .luaPendingItems = comm.luaPendingItems,
        .uiPendingItems = comm.uiPendingItems,
        .postprocessWorkerThreads = comm.postprocessWorkerThreads,
        .backlogWarning = comm.backlogWarning,
        .lastPumpEvents = comm.lastPumpEvents,
        .lastPumpRxBytes = comm.lastPumpRxBytes,
        .lastPumpStreamFrames = comm.lastPumpStreamFrames,
        .lastPumpStreamErrors = comm.lastPumpStreamErrors,
        .lastPumpTransportMs = comm.lastPumpTransportMs,
        .lastPumpParserMs = comm.lastPumpParserMs,
        .lastPumpCallbackMs = comm.lastPumpCallbackMs,
        .lastPumpScriptMs = comm.lastPumpScriptMs,
    };

    const auto hasPressure = [](const CommPressureDebugSnapshot& value) {
        return value.pendingRxBytes > 0U || value.pendingTransferFrameRows > 0U || value.pendingPlotAppends > 0U ||
               value.rxInputQueueBytes > 0U || value.parserPendingBytes > 0U || value.postprocessPendingBatches > 0U ||
               value.luaPendingItems > 0U || value.uiPendingItems > 0U || !value.backlogWarning.empty();
    };
    const auto pressureValuesChanged = [](const CommPressureDebugSnapshot& current,
                                          const CommPressureDebugSnapshot& previous) {
        return current.pendingRxBytes != previous.pendingRxBytes ||
               current.pendingTransferFrameRows != previous.pendingTransferFrameRows ||
               current.pendingPlotAppends != previous.pendingPlotAppends ||
               current.rxInputQueueBytes != previous.rxInputQueueBytes ||
               current.parserPendingBytes != previous.parserPendingBytes ||
               current.postprocessPendingBatches != previous.postprocessPendingBatches ||
               current.luaPendingItems != previous.luaPendingItems ||
               current.uiPendingItems != previous.uiPendingItems ||
               current.postprocessWorkerThreads != previous.postprocessWorkerThreads ||
               current.backlogWarning != previous.backlogWarning;
    };

    const bool active = hasPressure(snapshot);
    const bool previousActive = commPressureDebugLog_.hasSnapshot && hasPressure(commPressureDebugLog_.lastSnapshot);
    if (!active && !previousActive) {
        return;
    }

    const auto now = nowMs();
    const bool changed =
        !commPressureDebugLog_.hasSnapshot || pressureValuesChanged(snapshot, commPressureDebugLog_.lastSnapshot);
    const bool intervalElapsed = commPressureDebugLog_.lastLogMs == 0U ||
                                 now - commPressureDebugLog_.lastLogMs >= kCommPressureDebugLogIntervalMs;
    if (!changed && !intervalElapsed) {
        return;
    }

    // 调试日志节流：压力指标仍保留在状态结构中，但默认不再直接占用主界面。
    std::ostringstream message;
    message << std::fixed << std::setprecision(2);
    message << "通讯压力: rx_backlog=" << snapshot.pendingRxBytes << " bytes"
            << ", parser_backlog=" << snapshot.parserPendingBytes << " bytes"
            << ", frame_rows=" << snapshot.pendingTransferFrameRows << ", plot_appends=" << snapshot.pendingPlotAppends
            << ", rx_input=" << snapshot.rxInputQueueBytes << " bytes"
            << ", post_batches=" << snapshot.postprocessPendingBatches << ", lua_pending=" << snapshot.luaPendingItems
            << ", ui_pending=" << snapshot.uiPendingItems << ", post_threads=" << snapshot.postprocessWorkerThreads
            << ", last_pump_events=" << snapshot.lastPumpEvents << ", last_pump_rx=" << snapshot.lastPumpRxBytes
            << " bytes" << ", last_stream_frames=" << snapshot.lastPumpStreamFrames
            << ", last_stream_errors=" << snapshot.lastPumpStreamErrors << ", pump_ms=" << snapshot.lastPumpTransportMs
            << ", parser_ms=" << snapshot.lastPumpParserMs << ", lua_callback_ms=" << snapshot.lastPumpCallbackMs
            << ", script_ms=" << snapshot.lastPumpScriptMs
            << ", warning=" << (snapshot.backlogWarning.empty() ? "none" : snapshot.backlogWarning);
    loggingFacade_.debug("comm_pressure", message.str());

    commPressureDebugLog_.hasSnapshot = true;
    commPressureDebugLog_.lastSnapshot = std::move(snapshot);
    commPressureDebugLog_.lastLogMs = now;
}

bool Application::handleTransportEvents()
{
    resetTransportPumpMetrics();
    pullTransportEventsFromTransport();

    const auto startedAt = std::chrono::steady_clock::now();
    std::size_t processed = 0;
    std::size_t processedRxBytes = 0;
    const auto maxRxBytes = rxBytesPerPump();
    const bool changed = drainTransportEventQueues(startedAt, maxRxBytes, processed, processedRxBytes);
    auto& comm = dockStore_.commState();
    comm.lastPumpEvents = processed;
    comm.lastPumpTransportMs = elapsedMilliseconds(startedAt, std::chrono::steady_clock::now());
    return changed || !pendingTransportEvents_.empty() || !pendingRxByteChunks_.empty();
}

void Application::resetTransportPumpMetrics()
{
    auto& comm = dockStore_.commState();
    comm.lastPumpEvents = 0;
    comm.lastPumpRxBytes = 0;
    comm.lastPumpStreamFrames = 0;
    comm.lastPumpStreamErrors = 0;
    comm.lastPumpTransportMs = 0.0;
    comm.lastPumpParserMs = 0.0;
    comm.lastPumpCallbackMs = 0.0;
    comm.lastPumpScriptMs = 0.0;
}

void Application::pullTransportEventsFromTransport()
{
    if (!transport_) {
        return;
    }
    auto events = transport_->takeEvents();
    pendingTransportEvents_.insert(
        pendingTransportEvents_.end(), std::make_move_iterator(events.begin()), std::make_move_iterator(events.end()));
}

bool Application::drainTransportEventQueues(const std::chrono::steady_clock::time_point& startedAt,
                                            const std::size_t maxRxBytes,
                                            std::size_t& processedEvents,
                                            std::size_t& processedRxBytes)
{
    bool changed = false;
    while (processedEvents < kTransportEventsPerPump) {
        if (!pendingRxByteChunks_.empty()) {
            const auto remainingRxBudget = maxRxBytes > processedRxBytes ? maxRxBytes - processedRxBytes : 0U;
            if (remainingRxBudget == 0U) {
                break;
            }
            const auto before = pendingRxByteCount();
            changed = processPendingRxBytes(remainingRxBudget) || changed;
            const auto after = pendingRxByteCount();
            processedRxBytes += before >= after ? before - after : 0U;
            ++processedEvents;
            if (std::chrono::steady_clock::now() - startedAt >= kTransportEventBudget) {
                break;
            }
            continue;
        }

        if (pendingTransportEvents_.empty()) {
            break;
        }

        auto event = std::move(pendingTransportEvents_.front());
        pendingTransportEvents_.pop_front();

        changed = processTransportEvent(event) || changed;
        ++processedEvents;
        if (processedEvents >= kTransportEventsPerPump) {
            break;
        }
        if (std::chrono::steady_clock::now() - startedAt >= kTransportEventBudget) {
            break;
        }
    }
    return changed;
}

bool Application::processPendingRxBytes(const std::size_t maxBytes)
{
    if (pendingRxByteChunks_.empty() || maxBytes == 0U) {
        return false;
    }

    auto& pending = pendingRxByteChunks_.front();
    const auto remaining = pending.bytes.size() - pending.offset;
    const auto chunkSize = (std::min)(remaining, maxBytes);
    transport::TransportBytesEvent chunk{
        .context = pending.context,
        .bytes =
            std::vector<std::uint8_t>(pending.bytes.begin() + static_cast<std::ptrdiff_t>(pending.offset),
                                      pending.bytes.begin() + static_cast<std::ptrdiff_t>(pending.offset + chunkSize)),
    };

    // 核心流程：大 RX 事件拆成小块喂给脚本和 UI，避免单次 pump 长时间占住主线程。
    const bool changed = processTransportEvent(chunk);
    pending.offset += chunkSize;
    if (pending.offset >= pending.bytes.size()) {
        pendingRxByteChunks_.pop_front();
    }
    return changed;
}

void Application::enqueuePendingRxBytes(transport::TransportBytesEvent event)
{
    pendingRxByteChunks_.push_back(PendingRxBytes{
        .context = event.context,
        .bytes = std::move(event.bytes),
        .offset = 0,
    });
}

void Application::detachPendingRealtimeBacklogFromConnection()
{
    for (auto& pending : pendingRxByteChunks_) {
        pending.context.readyForIo = false;
    }

    std::deque<PendingRxBytes> detachedRxBytes;
    while (!pendingTransportEvents_.empty()) {
        auto event = std::move(pendingTransportEvents_.front());
        pendingTransportEvents_.pop_front();
        if (auto* bytes = std::get_if<transport::TransportBytesEvent>(&event);
            bytes != nullptr && !bytes->bytes.empty()) {
            bytes->context.readyForIo = false;
            detachedRxBytes.push_back(PendingRxBytes{
                .context = bytes->context,
                .bytes = std::move(bytes->bytes),
                .offset = 0,
            });
        }
    }
    pendingRxByteChunks_.insert(pendingRxByteChunks_.end(),
                                std::make_move_iterator(detachedRxBytes.begin()),
                                std::make_move_iterator(detachedRxBytes.end()));
}

Application::RealtimeBacklogDiscardCounts Application::clearPendingRealtimeBacklog()
{
    RealtimeBacklogDiscardCounts counts{
        .transportEvents = pendingTransportEvents_.size(),
        .rxBytes = pendingRxByteCount(),
        .transferFrameRows = pendingTransferFrameRows_.size(),
    };
    pendingTransportEvents_.clear();
    pendingRxByteChunks_.clear();
    pendingTransferFrameRows_.clear();
    counts.plotAppends += pendingScriptPlotAppends_.size();
    pendingScriptPlotAppends_.clear();

    const auto scriptCounts = scriptWorker_.clearPendingRealtimeOutputs();
    counts.plotAppends += scriptCounts.plotAppends;
    counts.scriptLogs = scriptCounts.logs;
    counts.scriptEvents = scriptCounts.events;
    return counts;
}

void Application::logRealtimeBacklogDiscard(const RealtimeBacklogDiscardCounts& counts)
{
    const auto total = counts.transportEvents + counts.rxBytes + counts.transferFrameRows + counts.plotAppends +
                       counts.scriptLogs + counts.scriptEvents;
    if (total == 0U) {
        return;
    }

    std::ostringstream message;
    message << "断开时已丢弃实时 UI backlog: transport_events=" << counts.transportEvents
            << ", rx_bytes=" << counts.rxBytes << ", transfer_frame_rows=" << counts.transferFrameRows
            << ", plot_appends=" << counts.plotAppends << ", script_logs=" << counts.scriptLogs
            << ", script_events=" << counts.scriptEvents;
    loggingFacade_.host(config::LogLevel::Warn, "BACKLOG_DROP", "realtime", message.str(), nowMs());
}

bool Application::responsiveBacklogMode() const
{
    return runtimeConfig_.gui.realtimeBacklog.mode != "complete";
}

std::size_t Application::rxBytesPerPump() const
{
    return (std::max<std::size_t>) (runtimeConfig_.gui.realtimeBacklog.rxChunkBytesPerPump, 1U);
}

std::size_t Application::transferFrameRowsPerPump() const
{
    return (std::max<std::size_t>) (runtimeConfig_.gui.realtimeBacklog.transferFrameRowsPerPump, 1U);
}

std::size_t Application::plotAppendsPerPump() const
{
    return (std::max<std::size_t>) (runtimeConfig_.gui.realtimeBacklog.plotAppendsPerPump, 1U);
}

std::size_t Application::pendingRxByteCount() const
{
    std::size_t total = 0;
    for (const auto& pending : pendingRxByteChunks_) {
        total += pending.bytes.size() - pending.offset;
    }
    for (const auto& event : pendingTransportEvents_) {
        if (const auto* bytes = std::get_if<transport::TransportBytesEvent>(&event); bytes != nullptr) {
            total += bytes->bytes.size();
        }
    }
    return total;
}

bool Application::hasPendingRequestDrainWork() const
{
    return !pendingTransportEvents_.empty() || !pendingRxByteChunks_.empty() || scriptWorker_.pendingRxBytes() > 0U;
}

bool Application::drainRequestTimeoutBacklog()
{
    if (!hasPendingRequestDrainWork()) {
        return false;
    }

    // 核心流程：request 已过脚本超时时间时，先把本地已到达的 RX/backlog 交给 parser。
    // 停止边界的 ACK 可能已经在 transport 或 worker 队列里，只是尚未产出 request_done。
    bool changed = handleTransportEvents();
    scriptWorker_.waitIdle();
    if (runtimeConfig_.scripting.drainRequestOutputsUnbounded) {
        changed = flushScriptOutputsUnbounded() || changed;
    } else {
        changed = flushScriptOutputs() || changed;
    }
    return true;
}

bool Application::processTransportEvent(const transport::TransportEvent& event)
{
    return std::visit(
        [this]<typename T0>(const T0& evt) {
            using T = std::decay_t<T0>;
            if constexpr (std::is_same_v<T, transport::TransportOpenEvent>) {
                return processTransportOpenEvent(evt);
            } else if constexpr (std::is_same_v<T, transport::TransportCloseEvent>) {
                return processTransportCloseEvent(evt);
            } else if constexpr (std::is_same_v<T, transport::TransportErrorEvent>) {
                return processTransportErrorEvent(evt);
            } else if constexpr (std::is_same_v<T, transport::TransportBytesEvent>) {
                return processTransportBytesEvent(evt);
            } else if constexpr (std::is_same_v<T, transport::TransportTxEvent>) {
                return processTransportTxEvent(evt);
            }
            return false;
        },
        event);
}

bool Application::processTransportOpenEvent(const transport::TransportOpenEvent& event)
{
    if (event.context.readyForIo) {
        activeConnection_ = event.context;
        scriptWorker_.postTransportOpen(event);
        scriptWorker_.waitIdle();
    }
    loggingFacade_.host(config::LogLevel::Info,
                        "OPEN",
                        event.context.endpoint,
                        "transport open" + transportContextFields("transport_open", event.context) +
                            " state=" + stateMessage(dockStore_.commState().state),
                        event.context.timestampMs);
    return true;
}

bool Application::processTransportCloseEvent(const transport::TransportCloseEvent& event)
{
    if (event.context.readyForIo) {
        scriptWorker_.postTransportClose(event);
        scriptWorker_.waitIdle();
    }
    loggingFacade_.host(config::LogLevel::Info,
                        "CLOSE",
                        event.context.endpoint,
                        "transport close" + transportContextFields("transport_close", event.context) +
                            " reason=" + event.reason,
                        event.context.timestampMs);
    resetStreamBufferAlertState(event.context.connectionId);
    if (activeConnection_.has_value() && activeConnection_->connectionId == event.context.connectionId) {
        activeConnection_.reset();
    }
    cancelAllTxRequests(event.reason.empty() ? "连接已关闭" : event.reason);
    std::string recordingError;
    if (rawCaptureRecording_.isOpen() && !stopRawCaptureRecording(recordingError)) {
        loggingFacade_.error("raw_capture", "停止完整原始数据录制失败: " + recordingError);
    }
    return true;
}

bool Application::processTransportErrorEvent(const transport::TransportErrorEvent& event)
{
    if (event.context.readyForIo) {
        scriptWorker_.postTransportError(event);
        scriptWorker_.waitIdle();
    }
    loggingFacade_.host(config::LogLevel::Error,
                        "ERROR",
                        event.context.endpoint,
                        "transport error" + transportContextFields("transport_error", event.context) +
                            " error=" + event.message,
                        event.context.timestampMs);
    dockStore_.commState().lastError = event.message;
    return true;
}

bool Application::processTransportBytesEvent(const transport::TransportBytesEvent& event)
{
    if (event.bytes.empty()) {
        return false;
    }
    // 核心流程：只消费当前活动连接的字节事件，旧连接的迟到回包直接忽略，
    // 避免双窗口接管场景下脚本状态与 UI 日志被过期连接污染。
    if (activeConnection_.has_value() && event.context.readyForIo &&
        activeConnection_->connectionId != event.context.connectionId) {
        return false;
    }
    appendRawCaptureRecording(event);
    appendLiveRawCapture(event);
    scriptWorker_.postTransportBytes(event);
    loggingFacade_.trace("transport",
                         "transport rx" + transportContextFields("transport_rx", event.context) +
                             " bytes=" + std::to_string(event.bytes.size()) +
                             payloadPreviewSuffix(loggingFacade_.currentConfig(), event.bytes));
    if (event.context.readyForIo) {
        activeConnection_ = event.context;
    }
    appendTransferRow(dock::ReceiveRow{
        .timestampMs = event.context.timestampMs,
        .direction = "RX",
        .endpoint = event.context.endpoint,
        .bytes = event.bytes,
        .message = {},
    });
    return true;
}

bool Application::processTransportTxEvent(const transport::TransportTxEvent& event)
{
    if (!activeWrite_.has_value() || activeWrite_->request.id != event.requestId) {
        return false;
    }

    auto activeWrite = *activeWrite_;
    activeWrite_.reset();
    const auto state = toScriptTxState(event.state);
    const std::optional<std::string> error =
        event.error.empty() ? std::nullopt : std::optional<std::string>{event.error};
    if (state == scripting::TxEventState::Sent && activeWrite.request.kind == scripting::TxRequestKind::Request) {
        activeWrite.sentAtMs = event.finishedAtMs;
        activeWrite.waitDeadlineMs = event.finishedAtMs + activeWrite.request.timeoutMs;
        activeHalfDuplexRequest_ = activeWrite;
        scriptWorker_.postRequestAwaitingCompletion(true);
        // 核心流程：先进入等待 ACK 状态再回调 on_tx，允许脚本在 sent 事件中立即调用 proto.request_done。
        scriptWorker_.postTxEvent(
            activeWrite.request.connection,
            scripting::TxEvent{
                .id = activeWrite.request.id,
                .kind = activeWrite.request.kind,
                .state = scripting::TxEventState::Sent,
                .tag = activeWrite.request.tag,
                .bytes = event.bytes,
                .queuedMs = activeWrite.request.createdAtMs,
                .finishedMs = event.finishedAtMs,
                .guarded = activeWrite.request.guarded,
                .attempt = activeWrite.request.attempt,
                .maxAttempts = activeWrite.request.maxAttempts,
                .guardState = guardStateForEvent(activeWrite.request, scripting::TxEventState::Sent),
                .error = error,
            });
        scriptWorker_.waitIdle();
        dockStore_.appendRequestTraceRow(
            makeRequestTraceRow(activeWrite.request, dock::RequestTraceState::Sent, error, event.finishedAtMs));
        loggingFacade_.trace("tx",
                             txRequestLogMessage("sent",
                                                 activeWrite.request,
                                                 pendingTxQueue_.size(),
                                                 std::nullopt,
                                                 event.finishedAtMs) +
                                 payloadPreviewSuffix(loggingFacade_.currentConfig(), activeWrite.request.payload));
        appendTransferRow(dock::ReceiveRow{
            .timestampMs = event.finishedAtMs,
            .direction = "TX",
            .endpoint = activeWrite.request.connection.endpoint,
            .bytes = activeWrite.request.payload,
            .message = {},
        });
        return true;
    }

    finishTxRequest(activeWrite.request, state, error, event.finishedAtMs);
    bool changed = true;
    changed = driveTxScheduler() || changed;
    return changed;
}

bool Application::applyScriptTransportStats(const scripting::ScriptRuntimeOutputBatch& batch)
{
    bool changed = false;
    if (batch.transportStats.has_value()) {
        const auto& stats = *batch.transportStats;
        auto& comm = dockStore_.commState();
        comm.lastPumpRxBytes += stats.bytes;
        comm.lastPumpStreamFrames += stats.streamFrames;
        comm.lastPumpStreamErrors += stats.streamErrors;
        comm.lastPumpParserMs += stats.parserMs;
        comm.lastPumpCallbackMs += stats.callbackMs;
        comm.lastPumpScriptMs += stats.totalMs;
        const auto snapshot = scriptWorker_.snapshot();
        if (snapshot.pendingWorkerRxBytes > 0U || snapshot.inputQueueSize > 0U || snapshot.outputQueueSize > 0U) {
            loggingFacade_.trace("worker",
                                 "worker backlog kind=worker_backlog queue_size=" +
                                     std::to_string(snapshot.inputQueueSize + snapshot.outputQueueSize) +
                                     " bytes=" + std::to_string(snapshot.pendingWorkerRxBytes) +
                                     " elapsed_ms=" + std::to_string(static_cast<std::uint64_t>(stats.totalMs)));
        }
        changed = true;
    }
    return changed;
}

bool Application::applyScriptTxOutputs(const scripting::ScriptRuntimeOutputBatch& batch)
{
    bool changed = false;
    for (const auto& connection : batch.requestGuardResets) {
        txRequestGuardHalted_ = false;
        const auto resetAtMs = nowMs();
        loggingFacade_.info("tx", "request guard reset" + transportContextFields("request_guard", connection));
        dockStore_.appendRequestTraceRow(dock::RequestTraceRow{
            .timestampMs = resetAtMs,
            .id = 0,
            .kind = dock::RequestTraceKind::Request,
            .state = dock::RequestTraceState::GuardReset,
            .endpoint = connection.endpoint,
            .tag = "request_guard",
            .queuedMs = resetAtMs,
            .finishedMs = resetAtMs,
            .guarded = true,
            .guardState = "reset",
            .error = {},
        });
        scriptWorker_.postTxEvent(connection,
                                  scripting::TxEvent{
                                      .id = 0,
                                      .kind = scripting::TxRequestKind::Request,
                                      .state = scripting::TxEventState::Completed,
                                      .tag = "request_guard",
                                      .queuedMs = resetAtMs,
                                      .finishedMs = resetAtMs,
                                      .guarded = true,
                                      .guardState = std::string("reset"),
                                  });
        scriptWorker_.waitIdle();
        changed = true;
    }

    for (auto request : batch.txRequests) {
        loggingFacade_.trace("tx",
                             txRequestLogMessage("received",
                                                 request,
                                                 pendingTxQueue_.size() +
                                                     (activeWrite_.has_value() ? 1U : 0U) +
                                                     (activeHalfDuplexRequest_.has_value() ? 1U : 0U)) +
                                 payloadPreviewSuffix(loggingFacade_.currentConfig(), request.payload));
        enqueueTxRequest(std::move(request));
        changed = true;
    }

    bool completionConsumed = false;
    for (const auto& result : batch.requestDoneResults) {
        if (completionConsumed) {
            loggingFacade_.warn("protocol", "同一轮收到多次 request_done，后续结果已忽略");
            changed = true;
            continue;
        }
        if (!activeHalfDuplexRequest_.has_value()) {
            loggingFacade_.warn("protocol", "收到 request_done，但当前没有活动 request");
            changed = true;
            continue;
        }
        const auto activeRequest = activeHalfDuplexRequest_->request;
        activeHalfDuplexRequest_.reset();
        scriptWorker_.postRequestAwaitingCompletion(false);
        if (result.ok && !result.message.empty() && dockStore_.commState().lastError == result.message) {
            dockStore_.commState().lastError.clear();
        }
        finishTxRequest(
            activeRequest,
            result.ok ? scripting::TxEventState::Completed : scripting::TxEventState::Failed,
            (!result.ok && !result.message.empty()) ? std::optional<std::string>{result.message} : std::nullopt,
            result.timestampMs);
        completionConsumed = true;
        changed = true;
    }
    return changed;
}

bool Application::applyScriptRuntimeProfileEvents(const scripting::ScriptRuntimeOutputBatch& batch)
{
    bool changed = false;
    for (const auto& profileEvent : batch.streamRuntimeProfiles) {
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
        if (rawCaptureRecording_.isOpen()) {
            std::string error;
            if (!rawCaptureRecording_.appendEvent(event, error)) {
                loggingFacade_.error("raw_capture", "录制 stream profile 事件失败: " + error);
            }
        }
        changed = true;
    }
    return changed;
}

bool Application::applyScriptUiAndLogOutputs(const scripting::ScriptRuntimeOutputBatch& batch)
{
    bool changed = false;
    for (const auto& update : batch.statusUpdates) {
        setStatusMessage(update.clear ? std::string{} : update.text, false);
        changed = true;
    }
    for (const auto& request : batch.dialogRequests) {
        enqueueDialogRequest(request);
        changed = true;
    }
    for (const auto& request : batch.fileDialogRequests) {
        pendingFileDialogs_.push_back(request);
        openFileDialogs_[request.id] = request;
        changed = true;
    }

    for (const auto& event : batch.events) {
        dockStore_.appendLuaEvent(event);
        changed = true;
    }
    for (const auto& log : batch.logs) {
        loggingFacade_.script(log.level, log.message, log.timestampMs);
        changed = true;
    }
    return changed;
}

bool Application::applyScriptOscilloscopeOutputs(const scripting::ScriptRuntimeOutputBatch& batch)
{
    bool changed = false;
    auto& wave = dockStore_.waveState();
    for (const auto& update : batch.oscilloscopeRunningUpdates) {
        if (wave.oscilloscopeRunning != update.running) {
            wave.oscilloscopeRunning = update.running;
            changed = true;
        }
    }
    return changed;
}

bool Application::applyScriptPlotOutputs(const scripting::ScriptRuntimeOutputBatch& batch)
{
    const bool setupChanged = applyScriptPlotSetups(batch.plotSetups);
    enqueueScriptPlotAppends(batch.plotAppends);
    auto appendRequests = drainScriptPlotAppendsForPump(plotAppendsPerPump());
    return appendScriptPlotRequestsToWave(std::move(appendRequests)) || setupChanged;
}

bool Application::applyScriptPlotSetups(const std::vector<scripting::PlotSetup>& setups)
{
    bool changed = false;
    for (const auto& setup : setups) {
        auto rawSetup = toRawPlotSetup(setup);
        applyPlotSetup(rawSetup);
        const auto timestampMs = activeConnection_.has_value() ? activeConnection_->timestampMs : nowMs();
        recordPlotSetupSnapshot(rawSetup, timestampMs);
        changed = true;
    }
    return changed;
}

void Application::enqueueScriptPlotAppends(const std::vector<std::pair<std::size_t, plot::WaveAppendRequest>>& appends)
{
    for (auto append : appends) {
        pendingScriptPlotAppends_.push_back(std::move(append));
    }
}

std::vector<std::pair<std::size_t, plot::WaveAppendRequest>> Application::drainScriptPlotAppendsForPump(
    const std::size_t maxPlotAppends)
{
    ScriptPlotAppendDrainState drainState{};
    drainState.maxSelectedKeys = maxPlotAppends;
    while (!pendingScriptPlotAppends_.empty()) {
        auto append = takePendingScriptPlotAppend(pendingScriptPlotAppends_);
        drainScriptPlotAppendCandidate(drainState, std::move(append));
    }

    // 保持预算外的不同源 append 原始顺序，留到后续 pump 继续处理。
    restoreDeferredScriptPlotAppends(pendingScriptPlotAppends_, drainState.deferred);
    return std::move(drainState.selected);
}

bool Application::appendScriptPlotRequestsToWave(std::vector<std::pair<std::size_t, plot::WaveAppendRequest>> requests)
{
    bool changed = false;
    auto mergedRequests = mergeNonEmptyScriptPlotAppends(std::move(requests));
    auto& wave = dockStore_.waveState();
    for (auto& [channelIndex, request] : mergedRequests) {
        if (wave.buffer.append(channelIndex, std::move(request))) {
            changed = true;
        }
    }
    return changed;
}

bool Application::applyScriptOutputBatch(const scripting::ScriptRuntimeOutputBatch& batch)
{
    bool changed = false;
    changed = applyScriptTransportStats(batch) || changed;
    changed = applyScriptTxOutputs(batch) || changed;
    changed = applyScriptRuntimeProfileEvents(batch) || changed;
    changed = applyScriptUiAndLogOutputs(batch) || changed;
    changed = applyScriptOscilloscopeOutputs(batch) || changed;
    changed = applyScriptPlotOutputs(batch) || changed;
    changed = driveTxScheduler() || changed;
    return changed;
}

bool Application::flushScriptOutputs()
{
    bool changed = false;
    const auto flushBudgetMs = runtimeConfig_.scripting.workerOutputFlushBudgetMs;
    std::uint64_t flushStartedAt = 0;
    if (flushBudgetMs > 0.0) {
        flushStartedAt = nowUs();
    }
    // 核心流程：逐批 drain，每次处理后检查时间预算，超时则停止
    // 剩余批次留在 worker 输出队列中，下次 pump 继续处理
    auto batch = scriptWorker_.drainOneOutput();
    while (batch.has_value()) {
        changed = applyScriptOutputBatch(*batch) || changed;
        if (flushBudgetMs > 0.0 && (nowUs() - flushStartedAt) >= static_cast<std::uint64_t>(flushBudgetMs * 1000.0)) {
            break;
        }
        batch = scriptWorker_.drainOneOutput();
    }
    return changed;
}

bool Application::flushScriptOutputsUnbounded()
{
    bool changed = false;
    auto batch = scriptWorker_.drainOneOutput();
    while (batch.has_value()) {
        changed = applyScriptOutputBatch(*batch) || changed;
        batch = scriptWorker_.drainOneOutput();
    }
    return changed;
}

bool Application::flushScriptStatusAndDialogs()
{
    return false;
}

bool Application::processScriptRequestCompletions()
{
    return false;
}

bool Application::processRequestTimeouts()
{
    if (!activeHalfDuplexRequest_.has_value()) {
        return false;
    }
    const auto currentMs = nowMs();
    if (currentMs < activeHalfDuplexRequest_->waitDeadlineMs) {
        return false;
    }

    if (drainRequestTimeoutBacklog()) {
        return true;
    }

    scriptWorker_.waitIdle();
    // 核心流程：request 已到超时边界时不能再受 UI 帧预算限制；
    // 已经到达的 ACK/request_done 必须先完整落到主线程，再决定是否真正超时。
    if (runtimeConfig_.scripting.drainRequestOutputsUnbounded) {
        flushScriptOutputsUnbounded();
    } else {
        flushScriptOutputs();
    }
    if (!activeHalfDuplexRequest_.has_value()) {
        return true;
    }

    auto request = activeHalfDuplexRequest_->request;
    activeHalfDuplexRequest_.reset();
    scriptWorker_.postRequestAwaitingCompletion(false);
    finishTxRequest(request, scripting::TxEventState::Timeout, std::string("等待 request_done 超时"), currentMs);
    if (request.guarded && request.attempt < request.maxAttempts) {
        // 核心流程：guarded request 的重试只属于当前请求；
        // 超时后把下一次 attempt 放回队首，避免后续请求抢在重试前发送。
        ++request.attempt;
        loggingFacade_.trace("tx",
                             txRequestLogMessage("retry_queued",
                                                 request,
                                                 pendingTxQueue_.size(),
                                                 std::nullopt,
                                                 currentMs) +
                                 payloadPreviewSuffix(loggingFacade_.currentConfig(), request.payload));
        dockStore_.appendRequestTraceRow(
            makeRequestTraceRow(request, dock::RequestTraceState::Queued, std::nullopt, currentMs));
        pendingTxQueue_.push_front(std::move(request));
    }
    driveTxScheduler();
    return true;
}

bool Application::driveTxScheduler()
{
    bool changed = false;
    while (!activeWrite_.has_value() && !activeHalfDuplexRequest_.has_value() && !pendingTxQueue_.empty()) {
        if (!transport_ || transport_->state() != transport::TransportState::Open) {
            auto request = std::move(pendingTxQueue_.front());
            pendingTxQueue_.pop_front();
            loggingFacade_.trace("tx",
                                 txRequestLogMessage("rejected",
                                                     request,
                                                     pendingTxQueue_.size(),
                                                     std::string_view("连接未打开"),
                                                     nowMs()));
            finishTxRequest(request, scripting::TxEventState::Rejected, std::string("连接未打开"), nowMs());
            changed = true;
            continue;
        }

        ActiveTxRequest active{};
        active.request = std::move(pendingTxQueue_.front());
        pendingTxQueue_.pop_front();

        if (active.request.guarded && txRequestGuardHalted_) {
            loggingFacade_.warn("tx",
                                txRequestLogMessage("guard_halted",
                                                    active.request,
                                                    pendingTxQueue_.size(),
                                                    std::string_view("guarded request 已熔断，请先调用 proto.reset_request_guard()"),
                                                    nowMs()));
            finishTxRequest(active.request,
                            scripting::TxEventState::Rejected,
                            std::string("guarded request 已熔断，请先调用 proto.reset_request_guard()"),
                            nowMs());
            changed = true;
            continue;
        }

        transport::TransportTxTask task{};
        task.requestId = active.request.id;
        task.kind = toTransportTxKind(active.request.kind);
        task.payload = active.request.payload;
        task.timeoutMs = active.request.timeoutMs;
        task.queuedAtMs = active.request.createdAtMs;
        if (!transport_->enqueueSend(std::move(task))) {
            loggingFacade_.warn("tx",
                                txRequestLogMessage("enqueue_failed",
                                                    active.request,
                                                    pendingTxQueue_.size(),
                                                    std::string_view("transport enqueueSend 失败"),
                                                    nowMs()));
            finishTxRequest(
                active.request, scripting::TxEventState::Rejected, std::string("transport enqueueSend 失败"), nowMs());
            changed = true;
            continue;
        }

        loggingFacade_.trace("tx",
                             txRequestLogMessage("sending",
                                                 active.request,
                                                 pendingTxQueue_.size(),
                                                 std::nullopt,
                                                 nowMs()) +
                                 payloadPreviewSuffix(loggingFacade_.currentConfig(), active.request.payload));
        activeWrite_ = std::move(active);
        changed = true;
    }
    return changed;
}

bool Application::enqueueTxRequest(scripting::TxRequest request)
{
    request.timeoutMs = request.timeoutMs > 0 ? request.timeoutMs
                                              : (request.kind == scripting::TxRequestKind::Request
                                                     ? runtimeConfig_.protocol.tx.requestTimeoutMs
                                                     : runtimeConfig_.protocol.tx.sendTimeoutMs);
    request.maxAttempts = std::max<std::uint32_t>(1U, request.maxAttempts);
    request.attempt = std::max<std::uint32_t>(1U, request.attempt);

    if (request.guarded && txRequestGuardHalted_) {
        loggingFacade_.warn("tx",
                            txRequestLogMessage("guard_halted",
                                                request,
                                                pendingTxQueue_.size(),
                                                std::string_view("guarded request 已熔断，请先调用 proto.reset_request_guard()"),
                                                nowMs()));
        finishTxRequest(request,
                        scripting::TxEventState::Rejected,
                        std::string("guarded request 已熔断，请先调用 proto.reset_request_guard()"),
                        nowMs());
        return true;
    }

    const std::size_t activeCount = pendingTxQueue_.size() + (activeWrite_.has_value() ? 1U : 0U) +
                                    (activeHalfDuplexRequest_.has_value() ? 1U : 0U);
    if (activeCount < runtimeConfig_.protocol.tx.maxPending) {
        loggingFacade_.trace("tx",
                             txRequestLogMessage("queued",
                                                 request,
                                                 activeCount + 1U,
                                                 std::nullopt,
                                                 nowMs()) +
                                 payloadPreviewSuffix(loggingFacade_.currentConfig(), request.payload));
        dockStore_.appendRequestTraceRow(
            makeRequestTraceRow(request, dock::RequestTraceState::Queued, std::nullopt, nowMs()));
        pendingTxQueue_.push_back(std::move(request));
        return true;
    }

    const std::string overflowMessage = "发送队列已满";
    if (runtimeConfig_.protocol.tx.overflowPolicy == "drop_oldest_waiting" && !pendingTxQueue_.empty()) {
        auto dropped = std::move(pendingTxQueue_.front());
        pendingTxQueue_.pop_front();
        loggingFacade_.warn("tx",
                            txRequestLogMessage("dropped",
                                                dropped,
                                                activeCount,
                                                std::string_view(overflowMessage),
                                                nowMs()));
        finishTxRequest(dropped, scripting::TxEventState::Dropped, overflowMessage, nowMs());
        loggingFacade_.trace("tx",
                             txRequestLogMessage("queued",
                                                 request,
                                                 activeCount,
                                                 std::nullopt,
                                                 nowMs()) +
                                 payloadPreviewSuffix(loggingFacade_.currentConfig(), request.payload));
        dockStore_.appendRequestTraceRow(
            makeRequestTraceRow(request, dock::RequestTraceState::Queued, std::nullopt, nowMs()));
        pendingTxQueue_.push_back(std::move(request));
        notifyTxOverflow(overflowMessage);
        return true;
    }

    if (runtimeConfig_.protocol.tx.overflowPolicy == "drop_newest") {
        loggingFacade_.warn("tx",
                            txRequestLogMessage("dropped",
                                                request,
                                                activeCount,
                                                std::string_view(overflowMessage),
                                                nowMs()));
        finishTxRequest(request, scripting::TxEventState::Dropped, overflowMessage, nowMs());
        notifyTxOverflow(overflowMessage);
        return true;
    }

    loggingFacade_.warn("tx",
                        txRequestLogMessage("rejected",
                                            request,
                                            activeCount,
                                            std::string_view(overflowMessage),
                                            nowMs()));
    finishTxRequest(request, scripting::TxEventState::Rejected, overflowMessage, nowMs());
    notifyTxOverflow(overflowMessage);
    return true;
}

void Application::cancelPendingTxRequests(const std::string& reason, std::uint64_t finishedAtMs)
{
    while (!pendingTxQueue_.empty()) {
        auto request = std::move(pendingTxQueue_.front());
        pendingTxQueue_.pop_front();
        finishTxRequest(request, scripting::TxEventState::Canceled, reason, finishedAtMs);
    }
}

void Application::finishTxRequest(const scripting::TxRequest& request,
                                  scripting::TxEventState state,
                                  std::optional<std::string> error,
                                  std::uint64_t finishedAtMs)
{
    if (isGuardedTerminalFailure(request, state)) {
        txRequestGuardHalted_ = true;
    }

    if (state == scripting::TxEventState::Canceled && activeHalfDuplexRequest_.has_value() &&
        activeHalfDuplexRequest_->request.id == request.id) {
        scriptWorker_.postRequestAwaitingCompletion(false);
    }

    dockStore_.appendRequestTraceRow(makeRequestTraceRow(request, toRequestTraceState(state), error, finishedAtMs));
    const auto stateText = txEventStateName(state);
    loggingFacade_.trace("tx",
                         txRequestLogMessage(stateText,
                                             request,
                                             pendingTxQueue_.size(),
                                             error.has_value() ? std::optional<std::string_view>{*error}
                                                               : std::nullopt,
                                             finishedAtMs) +
                             payloadPreviewSuffix(loggingFacade_.currentConfig(), request.payload));

    if (state == scripting::TxEventState::Sent) {
        appendTransferRow(dock::ReceiveRow{
            .timestampMs = finishedAtMs,
            .direction = "TX",
            .endpoint = request.connection.endpoint,
            .bytes = request.payload,
            .message = {},
        });
    }
    if (error.has_value()) {
        dockStore_.commState().lastError = *error;
        loggingFacade_.warn("protocol", *error);
    }

    scriptWorker_.postTxEvent(
        request.connection,
        scripting::TxEvent{
            .id = request.id,
            .kind = request.kind,
            .state = state,
            .tag = request.tag,
            .bytes = request.payload.size(),
            .queuedMs = request.createdAtMs,
            .finishedMs = finishedAtMs,
            .fileJobId = request.fileJobId,
            .offset = request.fileOffset,
            .total = request.fileTotal,
            .progress = request.fileTotal == 0 ? 0.0
                                               : static_cast<double>(request.fileOffset + request.payload.size()) /
                                                     static_cast<double>(request.fileTotal),
            .guarded = request.guarded,
            .attempt = request.attempt,
            .maxAttempts = request.maxAttempts,
            .guardState = guardStateForEvent(request, state),
            .error = std::move(error),
        });
    scriptWorker_.waitIdle();
}

void Application::cancelAllTxRequests(const std::string& reason)
{
    const auto finishedAtMs = nowMs();
    if (activeWrite_.has_value()) {
        finishTxRequest(activeWrite_->request, scripting::TxEventState::Canceled, reason, finishedAtMs);
        activeWrite_.reset();
    }
    if (activeHalfDuplexRequest_.has_value()) {
        scriptWorker_.postRequestAwaitingCompletion(false);
        finishTxRequest(activeHalfDuplexRequest_->request, scripting::TxEventState::Canceled, reason, finishedAtMs);
        activeHalfDuplexRequest_.reset();
    }
    cancelPendingTxRequests(reason, finishedAtMs);
}

void Application::notifyTxOverflow(const std::string& message)
{
    setStatusMessage(message, false);
    loggingFacade_.warn("protocol", message);
    if (runtimeConfig_.protocol.tx.overflowNotify == "popup_once") {
        const auto createdAtMs = static_cast<std::uint64_t>(nowMs());
        transport::ConnectionContext connection{};
        if (activeConnection_.has_value()) {
            connection = *activeConnection_;
        }
        const scripting::DialogRequest request{
            .id = createdAtMs,
            .kind = scripting::DialogKind::Alert,
            .connection = connection,
            .title = "发送队列已满",
            .message = message,
            .level = "warn",
            .dedupeKey = "protocol.tx.overflow",
            .createdAtMs = createdAtMs,
        };
        enqueueDialogRequest(request);
    }
}

void Application::handleStreamBufferAlert(const transport::ConnectionContext& context,
                                          const scripting::StreamParseBatch& batch,
                                          const scripting::StreamBufferDefinition& bufferDefinition)
{
    if (batch.bufferCapacity == 0 || batch.overflowed) {
        return;
    }

    const double thresholdRatio = runtimeConfig_.receive.streamBuffer.nearOverflowThreshold > 0.0
                                      ? runtimeConfig_.receive.streamBuffer.nearOverflowThreshold
                                      : bufferDefinition.nearOverflowThresholdRatio;
    const bool nearOverflow =
        static_cast<double>(batch.bufferSize) / static_cast<double>(batch.bufferCapacity) >= thresholdRatio;
    if (!nearOverflow) {
        return;
    }

    if (streamBufferAlertState_.connectionId != context.connectionId) {
        streamBufferAlertState_.connectionId = context.connectionId;
        streamBufferAlertState_.popupMuted = false;
        streamBufferAlertState_.popupOpen = false;
    }

    std::ostringstream status;
    status << "协议解析缓冲区占用过高: " << batch.bufferSize << "/" << batch.bufferCapacity << " bytes";
    setStatusMessage(status.str(), true);

    std::ostringstream logMessage;
    logMessage << status.str() << ", threshold=" << static_cast<int>(thresholdRatio * 100.0)
               << "%, endpoint=" << context.endpoint;
    loggingFacade_.warn("stream_buffer", logMessage.str());

    if (!bufferDefinition.popupEnabled || !runtimeConfig_.receive.streamBuffer.popupEnabled ||
        streamBufferAlertState_.popupMuted || streamBufferAlertState_.popupOpen) {
        return;
    }

    const auto createdAtMs = static_cast<std::uint64_t>(nowMs());
    const std::string thresholdText = std::to_string(static_cast<int>(thresholdRatio * 100.0));
    const scripting::DialogRequest request{
        .id = createdAtMs,
        .kind = scripting::DialogKind::Alert,
        .connection = context,
        .title = "协议解析缓冲区占用过高",
        .message = "当前连接 " + context.endpoint + " 的协议解析缓冲区已达到 " + std::to_string(batch.bufferSize) +
                   "/" + std::to_string(batch.bufferCapacity) + " bytes，告警阈值 " + thresholdText + "%。",
        .level = "warn",
        .dedupeKey = "receive.stream_buffer.near_overflow",
        .createdAtMs = createdAtMs,
    };
    enqueueDialogRequest(request);
    streamBufferAlertState_.popupOpen = true;
}

void Application::resetStreamBufferAlertState(const std::uint64_t connectionId)
{
    if (connectionId != 0 && streamBufferAlertState_.connectionId != connectionId) {
        return;
    }
    streamBufferAlertState_ = StreamBufferAlertState{};
}

void Application::enqueueDialogRequest(const scripting::DialogRequest& request)
{
    if (!request.dedupeKey.empty() && dialogDedupeKeys_.contains(request.dedupeKey)) {
        return;
    }
    pendingDialogs_.push_back(request);
    openDialogs_[request.id] = request;
    if (!request.dedupeKey.empty()) {
        dialogDedupeKeys_[request.dedupeKey] = request.id;
    }
}

bool Application::flushScriptLogs()
{
    return false;
}

void Application::enqueueTransferFrameRows(std::vector<dock::ReceiveRow> rows)
{
    if (rows.empty()) {
        return;
    }
    for (auto& row : rows) {
        pendingTransferFrameRows_.push_back(std::move(row));
    }
    trimPendingTransferFrameRowsToLimit();
}

void Application::trimPendingTransferFrameRowsToLimit()
{
    const auto limit = runtimeConfig_.gui.logHistory.transferFrameLimit;
    if (limit == 0U) {
        pendingTransferFrameRows_.clear();
        return;
    }
    const auto displayed = dockStore_.receiveState().frameRows.size();
    if (displayed + pendingTransferFrameRows_.size() <= limit) {
        return;
    }
    auto excess = displayed + pendingTransferFrameRows_.size() - limit;
    while (excess > 0 && !pendingTransferFrameRows_.empty()) {
        pendingTransferFrameRows_.pop_front();
        --excess;
    }
}

bool Application::flushPendingTransferFrameRows(std::size_t maxRows)
{
    if (pendingTransferFrameRows_.empty() || maxRows == 0) {
        return false;
    }
    std::vector<dock::ReceiveRow> rows;
    rows.reserve((std::min)(maxRows, pendingTransferFrameRows_.size()));
    while (!pendingTransferFrameRows_.empty() && rows.size() < maxRows) {
        rows.push_back(std::move(pendingTransferFrameRows_.front()));
        pendingTransferFrameRows_.pop_front();
    }
    dockStore_.appendTransferFrameRows(std::move(rows));
    trimPendingTransferFrameRowsToLimit();
    return true;
}

logging::LoggingFacade& Application::logger()
{
    return loggingFacade_;
}

const logging::LoggingFacade& Application::logger() const
{
    return loggingFacade_;
}

std::vector<scripting::DialogRequest> Application::drainDialogRequests()
{
    std::vector<scripting::DialogRequest> drained;
    drained.reserve(pendingDialogs_.size());
    while (!pendingDialogs_.empty()) {
        drained.push_back(std::move(pendingDialogs_.front()));
        pendingDialogs_.pop_front();
    }
    return drained;
}

void Application::respondDialog(const scripting::DialogEvent& event)
{
    const auto iter = openDialogs_.find(event.id);
    if (iter == openDialogs_.end()) {
        return;
    }
    const auto request = iter->second;
    if (request.dedupeKey == "receive.stream_buffer.near_overflow") {
        streamBufferAlertState_.popupOpen = false;
        if (event.state == "mute_until_disconnect") {
            streamBufferAlertState_.popupMuted = true;
            streamBufferAlertState_.connectionId = request.connection.connectionId;
        }
    }
    if (!request.dedupeKey.empty()) {
        dialogDedupeKeys_.erase(request.dedupeKey);
    }
    openDialogs_.erase(iter);
    scriptWorker_.postDialogEvent(request.connection, event);
}

std::vector<scripting::FileDialogRequest> Application::drainFileDialogRequests()
{
    std::vector<scripting::FileDialogRequest> drained;
    drained.reserve(pendingFileDialogs_.size());
    while (!pendingFileDialogs_.empty()) {
        drained.push_back(std::move(pendingFileDialogs_.front()));
        pendingFileDialogs_.pop_front();
    }
    return drained;
}

void Application::respondFileDialog(const scripting::FileDialogEvent& event)
{
    const auto iter = openFileDialogs_.find(event.id);
    if (iter == openFileDialogs_.end()) {
        return;
    }
    const auto request = iter->second;
    openFileDialogs_.erase(iter);
    scriptWorker_.postFileDialogEvent(request.connection, event);
}

bool Application::flushScriptPlots()
{
    return false;
}

} // namespace protoscope::app
