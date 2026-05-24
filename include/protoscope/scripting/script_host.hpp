#pragma once

#include "protoscope/protocol_utils/codec.hpp"
#include "protoscope/transport/transport.hpp"

#include <cstdint>
#include <deque>
#include <functional>
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

struct ScriptHostContext {
    transport::ConnectionContext connection;
};

class ScriptHost {
public:
    ScriptHost();

    bool loadScriptFile(const std::string& path);
    void resetRuntime();

    void onTransportOpen(const transport::TransportOpenEvent& event);
    void onTransportClose(const transport::TransportCloseEvent& event);
    void onTransportError(const transport::TransportErrorEvent& event);
    void onTransportBytes(const transport::TransportBytesEvent& event);

    void onControl(const transport::ConnectionContext& ctx, const std::string& id, const ControlValue& value);
    void invokeAction(const transport::ConnectionContext& ctx, const std::string& actionName);
    void tick(std::uint64_t nowMs);

    std::vector<ControlDescriptor> controlsSnapshot() const;
    std::vector<ScriptEvent> drainEvents();
    std::vector<ScriptLog> drainLogs();
    std::vector<std::vector<std::uint8_t>> drainSendQueue();

private:
    // 核心流程：下列方法模拟第一版 Lua 回调契约，后续可替换为 sol2 实际脚本调用。
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

    static std::uint64_t nowMs();
    static std::string valueToString(const ControlValue& value);

private:
    struct TimerState {
        std::string name;
        std::uint64_t dueAtMs{0};
        bool active{false};
    };

    bool scriptLoaded_{false};
    std::string scriptPath_;
    std::vector<ControlDescriptor> controls_;
    std::vector<ScriptEvent> events_;
    std::vector<ScriptLog> logs_;
    std::vector<std::vector<std::uint8_t>> sendQueue_;
    std::unordered_map<std::string, TimerState> timers_;

    bool waitingResponse_{false};
    std::string lastDeviceId_{"01"};
    std::optional<transport::ConnectionContext> activeConnection_;
};

} // namespace protoscope::scripting
