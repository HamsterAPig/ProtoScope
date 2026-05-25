#pragma once

#include "protoscope/plot/oscilloscope.hpp"
#include "protoscope/protocol_utils/codec.hpp"
#include "protoscope/transport/transport.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace protoscope::scripting {

enum class ControlType {
    Button,
    InputText,
    InputInt,
    InputFloat,
    Checkbox,
    Combo,
};

struct ControlDescriptor {
    ControlType type{ControlType::Button};
    std::string id;
    std::string label;
    std::string textDefault;
    int intDefault{0};
    float floatDefault{0.0F};
    bool boolDefault{false};
    std::vector<std::string> comboOptions;
    int comboDefaultIndex{0};
};

using ControlValue = std::variant<bool, int, float, std::string>;

struct ControlSnapshot {
    ControlDescriptor descriptor;
    ControlValue value;
};

struct DockDescriptor {
    std::string id;
    std::string title;
    std::vector<ControlDescriptor> controls;
};

struct DockSnapshot {
    DockDescriptor descriptor;
    std::vector<ControlSnapshot> controls;
};

struct ScriptEvent {
    std::string name;
    std::string payload;
    std::uint64_t timestampMs{0};
};

struct ScriptLog {
    std::string level;
    std::string message;
    std::uint64_t timestampMs{0};
};

struct PlotChannelDescriptor {
    std::string label;
    std::string unit;
};

struct PlotSetup {
    std::string source;
    std::vector<PlotChannelDescriptor> channels;
    plot::ViewConfig view{};
    bool resetHistory{false};
};

struct ScriptHostContext {
    transport::ConnectionContext connection;
};

class ScriptHost {
public:
    ScriptHost();
    ~ScriptHost();

    bool loadScriptFile(const std::string& path);
    bool loadProtocolDirectory(const std::string& directory);
    void resetRuntime();

    void onTransportOpen(const transport::TransportOpenEvent& event);
    void onTransportClose(const transport::TransportCloseEvent& event);
    void onTransportError(const transport::TransportErrorEvent& event);
    void onTransportBytes(const transport::TransportBytesEvent& event);
    void onControl(const transport::ConnectionContext& ctx, const std::string& id, const ControlValue& value);
    void tick(std::uint64_t currentMs);

    std::vector<ControlDescriptor> controlsSnapshot() const;
    std::vector<ControlSnapshot> controlStatesSnapshot() const;
    std::vector<DockDescriptor> dockDescriptorsSnapshot() const;
    std::vector<DockSnapshot> dockSnapshots() const;
    std::vector<ScriptEvent> drainEvents();
    std::vector<ScriptLog> drainLogs();
    std::vector<std::vector<std::uint8_t>> drainSendQueue();
    std::vector<PlotSetup> drainPlotSetups();
    std::vector<std::pair<std::size_t, plot::WaveAppendRequest>> drainPlotAppends();
    std::optional<std::uint64_t> nextWakeupAtMs() const;

    const std::string& scriptPath() const;
    const std::string& protocolDirectory() const;
    const std::string& lastError() const;

private:
    void callbackOnOpen(const ScriptHostContext& ctx);
    void callbackOnClose(const ScriptHostContext& ctx);
    void callbackOnError(const ScriptHostContext& ctx, const std::string& message);
    void callbackOnBytes(const ScriptHostContext& ctx, const std::vector<std::uint8_t>& bytes);
    void callbackOnTimer(const ScriptHostContext& ctx, const std::string& timerName);
    void callbackOnControl(const ScriptHostContext& ctx, const std::string& id, const ControlValue& value);

    void protoSend(const std::vector<std::uint8_t>& bytes);
    void protoLog(const std::string& level, const std::string& message);
    void protoEmit(const std::string& eventName, const std::string& payload);
    void protoSetTimer(const std::string& name, std::uint64_t intervalMs);
    void protoCancelTimer(const std::string& name);
    void protoPlotSetup(const PlotSetup& setup);
    void protoPlotPush(std::size_t channelIndex, const plot::WaveAppendRequest& request);

    static std::string valueToString(const ControlValue& value);
    void setLastError(std::string message);

    struct Runtime;

private:
    struct TimerState {
        std::string name;
        std::uint64_t dueAtMs{0};
        bool active{false};
    };

    bool scriptLoaded_{false};
    std::string scriptPath_;
    std::string protocolDirectory_;
    std::string lastError_;
    std::vector<DockDescriptor> docks_;
    std::vector<ControlDescriptor> controls_;
    std::unordered_map<std::string, ControlValue> controlValues_;
    std::vector<ScriptEvent> events_;
    std::vector<ScriptLog> logs_;
    std::vector<std::vector<std::uint8_t>> sendQueue_;
    std::vector<PlotSetup> plotSetups_;
    std::vector<std::pair<std::size_t, plot::WaveAppendRequest>> plotAppends_;
    std::unordered_map<std::string, TimerState> timers_;
    std::optional<transport::ConnectionContext> activeConnection_;
    std::unique_ptr<Runtime> runtime_;
};

} // namespace protoscope::scripting
