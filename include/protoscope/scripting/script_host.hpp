#pragma once

#include "protoscope/plot/oscilloscope.hpp"
#include "protoscope/protocol_utils/codec.hpp"
#include "protoscope/scripting/file_io_config.hpp"
#include "protoscope/scripting/frame_stream_parser.hpp"
#include "protoscope/transport/transport.hpp"

#include <sol/sol.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace protoscope::scripting {

class CodecScriptHostApiModule;
class ControlScriptHostApiModule;
class CoreScriptHostApiModule;
class FileScriptHostApiModule;
class PlotScriptHostApiModule;
class StatusScriptHostApiModule;
class TxScriptHostApiModule;
class UiScriptHostApiModule;

enum class ControlType {
    Button,
    InputText,
    InputInt,
    InputFloat,
    Checkbox,
    Combo,
    ElfSymbolCombo,
};

struct ElfSymbolValue {
    std::string label;
    std::string value;
    std::string type;
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
    int debounceMs{150};
    std::size_t limit{64};
    bool debounceMsConfigured{false};
    bool limitConfigured{false};
};

using ControlValue = std::variant<bool, int, float, std::string, ElfSymbolValue>;

struct ControlSnapshot {
    ControlDescriptor descriptor;
    ControlValue value;
};

enum class DockLayoutKind {
    Flow,
    Table,
    Form,
};

struct TableCellDescriptor {
    std::string controlId;
    bool spacer{false};
};

struct TableRowDescriptor {
    std::vector<TableCellDescriptor> cells;
};

struct TableLayoutDescriptor {
    std::size_t columns{1};
    bool borders{false};
    bool resizable{true};
    bool rowBg{false};
    std::string sizing{"stretch"};
    std::vector<TableRowDescriptor> rows;
};

struct FormControlRowDescriptor {
    std::vector<std::string> controlIds;
};

struct FormTextDescriptor {
    std::string text;
};

struct FormSeparatorDescriptor {
};

enum class FormLayoutItemKind {
    Control,
    Controls,
    Group,
    Collapse,
    Separator,
    Text,
};

struct FormGroupDescriptor;
struct FormCollapseDescriptor;

struct FormLayoutItemDescriptor {
    FormLayoutItemKind kind{FormLayoutItemKind::Control};
    std::string controlId;
    FormControlRowDescriptor controls;
    std::shared_ptr<FormGroupDescriptor> group;
    std::shared_ptr<FormCollapseDescriptor> collapse;
    FormTextDescriptor text;
    FormSeparatorDescriptor separator;
};

struct FormGroupDescriptor {
    std::string title;
    std::vector<FormLayoutItemDescriptor> items;
};

struct FormCollapseDescriptor {
    std::string title;
    bool defaultOpen{true};
    std::vector<FormLayoutItemDescriptor> items;
};

struct FormLayoutDescriptor {
    std::vector<FormLayoutItemDescriptor> items;
};

struct DockLayoutDescriptor {
    DockLayoutKind kind{DockLayoutKind::Flow};
    TableLayoutDescriptor table;
    FormLayoutDescriptor form;
};

struct DockDescriptor {
    std::string id;
    std::string title;
    std::string anchor{"left_bottom"};
    std::string tabGroup;
    std::vector<ControlDescriptor> controls;
    std::optional<DockLayoutDescriptor> layout;
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
    double ratio{1.0};
    double scale{1.0};
    double offset{0.0};
    std::optional<std::array<float, 4>> color;
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

enum class TxRequestKind {
    Send,
    Request,
};

struct ProtoBuffer {
    std::vector<std::uint8_t> bytes;

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] ProtoBuffer slice(std::size_t offset, std::size_t size) const;
    [[nodiscard]] std::string toHex(std::size_t maxBytes = 0) const;
};

struct TxRequest {
    std::uint64_t id{0};
    TxRequestKind kind{TxRequestKind::Send};
    transport::ConnectionContext connection{};
    std::vector<std::uint8_t> payload;
    std::uint64_t timeoutMs{1000};
    std::string tag;
    std::uint64_t createdAtMs{0};
    std::uint64_t fileJobId{0};
    std::uint64_t fileOffset{0};
    std::uint64_t fileTotal{0};
};

enum class TxEventState {
    Sent,
    Completed,
    Timeout,
    Rejected,
    Dropped,
    Canceled,
};

struct TxEvent {
    std::uint64_t id{0};
    TxRequestKind kind{TxRequestKind::Send};
    TxEventState state{TxEventState::Sent};
    std::string tag{};
    std::size_t bytes{0};
    std::uint64_t queuedMs{0};
    std::uint64_t finishedMs{0};
    std::uint64_t fileJobId{0};
    std::uint64_t offset{0};
    std::uint64_t total{0};
    double progress{0.0};
    std::optional<std::string> error{};
};

struct RequestDoneResult {
    bool ok{true};
    std::string message;
    std::uint64_t timestampMs{0};
};

struct StatusUpdate {
    std::string text;
    std::string level{"info"};
    bool clear{false};
    std::uint64_t timestampMs{0};
};

enum class DialogKind {
    Alert,
    Confirm,
};

enum class FileDialogKind {
    OpenFile,
    SaveFile,
    OpenDir,
};

struct FileDialogFilter {
    std::string name{};
    std::vector<std::string> patterns{};
};

struct FileDialogRequest {
    std::uint64_t id{0};
    FileDialogKind kind{FileDialogKind::OpenFile};
    transport::ConnectionContext connection{};
    std::string title{};
    std::string defaultPath{};
    std::vector<FileDialogFilter> filters{};
    std::uint64_t createdAtMs{0};
};

struct FileDialogEvent {
    std::uint64_t id{0};
    FileDialogKind kind{FileDialogKind::OpenFile};
    std::string state{};
    std::string path{};
    std::string error{};
    std::uint64_t timestampMs{0};
};

struct DialogRequest {
    std::uint64_t id{0};
    DialogKind kind{DialogKind::Alert};
    transport::ConnectionContext connection{};
    std::string title{};
    std::string message{};
    std::string level{"info"};
    std::string dedupeKey{};
    std::uint64_t createdAtMs{0};
};

struct DialogEvent {
    std::uint64_t id{0};
    DialogKind kind{DialogKind::Alert};
    std::string state{};
    std::optional<bool> confirmed{};
    std::string title{};
    std::string message{};
    std::string level{"info"};
    std::string dedupeKey{};
    std::uint64_t timestampMs{0};
};

struct RealtimeOutputDiscardCounts {
    std::size_t events{0};
    std::size_t logs{0};
    std::size_t plotAppends{0};
};

class ScriptHost {
public:
    ScriptHost();
    ~ScriptHost();

    bool loadScriptFile(const std::string& path);
    bool loadProtocolDirectory(const std::string& directory);
    void setFileIoConfig(FileIoConfig config);
    void resetRuntime();

    void onTransportOpen(const transport::TransportOpenEvent& event);
    void onTransportClose(const transport::TransportCloseEvent& event);
    void onTransportError(const transport::TransportErrorEvent& event);
    void onTransportBytes(const transport::TransportBytesEvent& event);
    void onControl(const transport::ConnectionContext& ctx, const std::string& id, const ControlValue& value);
    bool setControlValue(const std::string& id, const ControlValue& value);
    void tick(std::uint64_t currentMs);

    std::vector<ControlDescriptor> controlsSnapshot() const;
    std::vector<ControlSnapshot> controlStatesSnapshot() const;
    std::vector<DockDescriptor> dockDescriptorsSnapshot() const;
    std::vector<DockSnapshot> dockSnapshots() const;
    std::vector<ScriptEvent> drainEvents();
    std::vector<ScriptLog> drainLogs();
    std::vector<TxRequest> drainTxRequests();
    std::vector<PlotSetup> drainPlotSetups();
    std::vector<std::pair<std::size_t, plot::WaveAppendRequest>> drainPlotAppends();
    std::vector<std::pair<std::size_t, plot::WaveAppendRequest>> drainPlotAppends(std::size_t maxRequests);
    [[nodiscard]] std::size_t pendingPlotAppendCount() const;
    RealtimeOutputDiscardCounts clearPendingRealtimeOutputs();
    std::vector<RequestDoneResult> drainRequestDoneResults();
    std::vector<StatusUpdate> drainStatusUpdates();
    std::vector<DialogRequest> drainDialogRequests();
    std::vector<FileDialogRequest> drainFileDialogRequests();
    std::optional<std::uint64_t> nextWakeupAtMs() const;
    [[nodiscard]] std::optional<StreamBufferDefinition> streamBufferDefinition() const;
    [[nodiscard]] std::vector<StreamFrameDefinition> streamFrameDefinitions() const;

    const std::string& scriptPath() const;
    const std::string& protocolDirectory() const;
    const std::string& lastError() const;

    void onTxEvent(const transport::ConnectionContext& ctx, const TxEvent& event);
    void onDialogEvent(const transport::ConnectionContext& ctx, const DialogEvent& event);
    void onFileDialogEvent(const transport::ConnectionContext& ctx, const FileDialogEvent& event);
    void setRequestAwaitingCompletion(bool active);

private:
    sol::state& luaState();
    sol::state_view luaView();
    const std::vector<ControlDescriptor>& controlDescriptors() const;
    const ControlValue* findControlValue(const std::string& id) const;
    void updateControlValue(const std::string& id, ControlValue value);

    void callbackOnOpen(const ScriptHostContext& ctx);
    void callbackOnClose(const ScriptHostContext& ctx);
    void callbackOnError(const ScriptHostContext& ctx, const std::string& message);
    void callbackOnBytes(const ScriptHostContext& ctx, const std::vector<std::uint8_t>& bytes);
    void callbackOnStreamFrame(const ScriptHostContext& ctx, const StreamParsedFrame& frame);
    void callbackOnStreamError(const ScriptHostContext& ctx, const StreamParseError& error);
    void callbackOnTimer(const ScriptHostContext& ctx, const std::string& timerName);
    void callbackOnControl(const ScriptHostContext& ctx, const std::string& id, const ControlValue& value);
    void callbackOnTx(const ScriptHostContext& ctx, const TxEvent& event);
    void callbackOnDialog(const ScriptHostContext& ctx, const DialogEvent& event);
    void callbackOnFileDialog(const ScriptHostContext& ctx, const FileDialogEvent& event);

    void registerLuaApi(sol::table& proto);

    std::optional<TxRequest> protoSendLike(TxRequestKind kind,
                                           const sol::object& payload,
                                           const sol::object& opts,
                                           std::string& error);
    void protoLog(const std::string& level, const std::string& message);
    void protoEmit(const std::string& eventName, const std::string& payload);
    void protoSetTimer(const std::string& name, std::uint64_t intervalMs);
    void protoCancelTimer(const std::string& name);
    void protoPlotSetup(const PlotSetup& setup);
    void protoPlotPush(std::size_t channelIndex, const plot::WaveAppendRequest& request);
    bool protoRequestDone(const sol::object& result, std::string& error);
    void protoStatusSet(const std::string& text, const sol::object& opts);
    void protoStatusClear();
    std::optional<DialogRequest> protoDialog(DialogKind kind, const sol::object& opts, std::string& error);
    std::optional<FileDialogRequest> protoFileDialog(FileDialogKind kind, const sol::object& opts, std::string& error);
    std::tuple<sol::object, sol::object> protoFsOpen(const std::string& path, const sol::object& opts);
    std::tuple<sol::object, sol::object> protoFsRead(std::uint64_t handle, const sol::object& opts);
    std::tuple<sol::object, sol::object> protoFsWrite(std::uint64_t handle, const sol::object& payload);
    std::tuple<sol::object, sol::object> protoFsClose(std::uint64_t handle);
    std::tuple<sol::object, sol::object> protoFsStat(const std::string& path);
    std::tuple<sol::object, sol::object> protoFsSendFile(const std::string& path, const sol::object& opts);
    std::uint64_t nextTxRequestId();
    std::uint64_t nextDialogId();
    std::uint64_t nextFileDialogId();
    std::uint64_t nextFileHandleId();
    std::uint64_t nextFileJobId();
    void pumpFileSendJob(std::uint64_t jobId);

    static std::string valueToString(const ControlValue& value);
    void setLastError(std::string message);

    friend class CodecScriptHostApiModule;
    friend class ControlScriptHostApiModule;
    friend class CoreScriptHostApiModule;
    friend class FileScriptHostApiModule;
    friend class PlotScriptHostApiModule;
    friend class StatusScriptHostApiModule;
    friend class TxScriptHostApiModule;
    friend class UiScriptHostApiModule;

    struct Runtime;
    struct FileHandle;
    struct AuthorizedPath;
    struct FileSendJob;

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
    std::vector<TxRequest> txRequests_;
    std::vector<PlotSetup> plotSetups_;
    std::vector<std::pair<std::size_t, plot::WaveAppendRequest>> plotAppends_;
    std::vector<RequestDoneResult> requestDoneResults_;
    std::vector<StatusUpdate> statusUpdates_;
    std::vector<DialogRequest> dialogRequests_;
    std::vector<FileDialogRequest> fileDialogRequests_;
    std::unordered_map<std::string, TimerState> timers_;
    std::unordered_map<std::uint64_t, std::unique_ptr<FileHandle>> fileHandles_;
    std::unordered_map<std::uint64_t, FileSendJob> fileSendJobs_;
    std::vector<AuthorizedPath> dialogAuthorizedPaths_;
    std::optional<transport::ConnectionContext> activeConnection_;
    std::unique_ptr<Runtime> runtime_;
    FileIoConfig fileIoConfig_{};
    std::uint64_t nextTxRequestId_{1};
    std::uint64_t nextDialogId_{1};
    std::uint64_t nextFileDialogId_{1};
    std::uint64_t nextFileHandleId_{1};
    std::uint64_t nextFileJobId_{1};
    bool requestAwaitingCompletion_{false};
};

} // namespace protoscope::scripting
