#include "protoscope/app/application.hpp"
#include "protoscope/plot/csv_data_file.hpp"
#include "protoscope/protocol_utils/codec.hpp"
#include "protoscope/dock/docks.hpp"
#include "test_helpers.hpp"

#include <chrono>
#include <algorithm>
#include <iostream>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

using namespace protoscope;
using Clock = std::chrono::steady_clock;

class WriteProbe final : public transport::TransportBase {
public:
    std::size_t writeLimit{2};
    std::size_t writeDelayMs{0};
    bool open(const transport::TransportConfig&) override {
        setState(transport::TransportState::Open);
        pushEvent(transport::TransportOpenEvent{*context_});
        return true;
    }
    void close() override { setState(transport::TransportState::Closed); }
    bool send(std::vector<std::uint8_t> bytes) override {
        const auto written = (std::min)(writeLimit, bytes.size());
        recordWrite(*context_, bytes, written, "sent");
        return written == bytes.size();
    }
    bool enqueueSend(transport::TransportTxTask task) override {
        const bool accepted = enqueueSendCommon(std::move(task), context_, io_, stopping_, [&](const auto& bytes) {
            std::this_thread::sleep_for(std::chrono::milliseconds(writeDelayMs));
            const auto written = (std::min)(writeLimit, bytes.size());
            return std::pair<std::size_t, std::string>{written, written < bytes.size() ? "write failed" : ""};
        });
        io_.restart();
        io_.poll();
        return accepted;
    }
private:
    std::optional<transport::ConnectionContext> context_{
        transport::ConnectionContext{.endpoint = "probe", .connectionId = 1, .timestampMs = 1}};
    asio::io_context io_;
    std::atomic<bool> stopping_{false};
};

static void require(bool value, const std::string& message)
{
    if (!value) throw std::runtime_error(message);
}

namespace protoscope::app {
// 复用已有测试友元；验证独立离线 parser 不被切换入口替换，不调用 Lua。
struct ApplicationBackpressureTestAccess {
    static void restoreBrowseSchema(Application& application) { application.resetTransferFrameParser(); }
    static bool historyActive(const Application& application) { return application.historyRebuild_.has_value(); }
    static bool historyOutputPending(const Application& application) { return !application.pendingTransferFrameRows_.empty(); }
    static void verifyReloadHistoryDeferred(const Application& application, bool beforePump)
    {
        if (!application.historyRebuild_)
            throw std::runtime_error("协议重载大历史必须保留有界任务");
        const auto& task = *application.historyRebuild_;
        if (beforePump && (task.eventCursor != 0 || task.byteOffset != 0 || !task.rebuilt.empty()))
            throw std::runtime_error("协议重载返回前不得复制并同步解析历史");
        if (!beforePump && (task.eventCursor > 1 || task.byteOffset > 64 * 1024))
            throw std::runtime_error("协议重载历史每轮必须受字节预算限制");
    }
    static void verifyHistoryBudgetsAndHandoff(Application& application)
    {
        scripting::StreamFrameDefinition frame;
        frame.name = "offline_profile"; frame.header = {0xFA}; frame.runtimeProfile = true;
        scripting::FrameStreamParser parser(scripting::StreamBufferDefinition{.capacity = 4096}, {frame});
        application.transferFrameParser_ = Application::TransferFrameParserState{.rx = parser, .tx = parser};
        auto& capture = application.dockStore_.waveState().rawCapture;
        capture = {};
        capture.source = "budget";
        capture.events = {
            {.type = plot::RawCaptureEventType::ProfileSet, .profile = {.frameName = frame.name, .length = 3}, .displayPosition = 1},
            {.type = plot::RawCaptureEventType::RxBytes, .bytes = {0xFA, 0x01, 0x02}, .displayPosition = 2},
            {.type = plot::RawCaptureEventType::RxBytes, .bytes = {0xFA, 0x03}, .displayPosition = 3}};
        application.dockStore_.clearReceiveRows();
        application.rememberTransferProfile(capture.events.front());
        application.appendHistoryReceiveRow({.direction = "RX", .bytes = {0xFA, 0x03}, .displayPosition = 3});
        const auto retainedCapture = capture.events;
        capture.events.clear(); // 底层已裁剪，面板仍保留：不能借底层字节或 profile 重建。
        application.beginHistoryRebuild(config::TransferLogHistoryScope::RetainedRows,
                                        application.historySwitchGeneration(), 1);
        application.appendTransferRow({.direction = "RX", .bytes = {0x04}, .displayPosition = 4});
        for (int i = 0; i < 100 && application.historyRebuild_; ++i) application.pumpHistoryRebuild();
        if (application.historyRebuild_ || application.dockStore_.receiveState().frameRows.size() != 1 ||
            application.dockStore_.receiveState().frameRows.front().bytes != std::vector<std::uint8_t>({0xFA, 0x03, 0x04}))
            throw std::runtime_error("面板范围只解析保留字节，折叠起点 profile，历史半帧和新字节连续生成一帧");
        // 面板已清空而底层仍保留，完整范围仍能恢复；不会重放面板已裁剪字节到面板范围。
        capture.events = retainedCapture;
        application.dockStore_.clearReceiveRows();
        // 完整范围不得复制旧 pending 再追加两次。
        application.pendingTransferFrameRows_.push_back({.bytes = {0x99}});
        application.beginHistoryRebuild(config::TransferLogHistoryScope::RetainedCapture,
                                        application.historySwitchGeneration(), 3);
        for (int i = 0; i < 100 && application.historyRebuild_; ++i) application.pumpHistoryRebuild();
        if (application.dockStore_.receiveState().frameRows.size() != 1 || !application.pendingTransferFrameRows_.empty())
            throw std::runtime_error("完整范围替换结果不能重复旧 pending");
        // 完成后的历史尾半帧直接交接给同一 UI parser。
        application.appendTransferFrameRows({.direction = "RX", .bytes = {0x04}});
        application.flushPendingTransferFrameRows(256);
        if (application.dockStore_.receiveState().frameRows.size() != 2 ||
            application.dockStore_.receiveState().frameRows.back().bytes != std::vector<std::uint8_t>({0xFA, 0x03, 0x04}))
            throw std::runtime_error("历史完成必须交接尾半帧和 profile，新字节只解析一次");
        // 一个超大事件必须带 offset 留到下一轮，不能一次解析全部字节。
        capture.events[1].bytes.assign(256 * 1024, 0);
        application.beginHistoryRebuild(config::TransferLogHistoryScope::RetainedCapture,
                                        application.historySwitchGeneration(), 3);
        application.pumpHistoryRebuild();
        if (!application.historyRebuild_ || application.historyRebuild_->eventCursor > 1 ||
            application.historyRebuild_->byteOffset > 64 * 1024)
            throw std::runtime_error("单大事件每轮必须受 64KiB 切片预算约束");
        application.cancelHistorySwitch(); // modal 取消不操作任务，此处显式来源取消。
        application.invalidateHistorySwitchWork("测试取消");
        // legacy payload 没有事件仍必须可用完整范围重建。
        capture.events.clear(); capture.payload = {0xFA, 0x01, 0x02};
        frame.runtimeProfile = false; frame.size = 3;
        scripting::FrameStreamParser fixed(scripting::StreamBufferDefinition{.capacity = 4096}, {frame});
        application.transferFrameParser_ = Application::TransferFrameParserState{.rx = fixed, .tx = fixed};
        application.beginHistoryRebuild(config::TransferLogHistoryScope::RetainedCapture,
                                        application.historySwitchGeneration(), 0);
        for (int i = 0; i < 100 && application.historyRebuild_; ++i) application.pumpHistoryRebuild();
        if (application.dockStore_.receiveState().frameRows.size() != 1)
            throw std::runtime_error("legacy payload 必须进入完整范围历史");
        // timeline 控制事件与行数不同；面板只选位置2，未来位置4不解析。
        application.rawCaptureReplay_.loaded = true;
        application.rawCaptureReplay_.processing = Application::OfflineReplayProcessing::PackageProtocol;
        frame.runtimeProfile = true; frame.size.reset();
        scripting::FrameStreamParser offline(scripting::StreamBufferDefinition{.capacity = 4096}, {frame});
        application.transferFrameParser_ = Application::TransferFrameParserState{.rx = offline, .tx = offline};
        application.rawCaptureReplay_.capture.events = {
            {.type = plot::RawCaptureEventType::ProfileSet, .profile = {.frameName = "offline_profile", .length = 3}, .displayPosition = 1},
            {.type = plot::RawCaptureEventType::RxBytes, .bytes = {0xFA, 0x05, 0x06}, .displayPosition = 2},
            {.type = plot::RawCaptureEventType::PlotSetup, .displayPosition = 3},
            {.type = plot::RawCaptureEventType::RxBytes, .bytes = {0xFA, 0x07, 0x08}, .displayPosition = 4}};
        application.rawCaptureReplay_.eventIndex = 3;
        application.dockStore_.clearReceiveRows();
        application.rememberTransferProfile(application.rawCaptureReplay_.capture.events.front());
        application.appendHistoryReceiveRow({.direction = "RX", .bytes = {0xFA, 0x05, 0x06}, .displayPosition = 2});
        application.beginHistoryRebuild(config::TransferLogHistoryScope::RetainedRows,
                                        application.historySwitchGeneration(), 1);
        // 重建期间 profile 与 TX 必须顺序追赶，并对两个方向生效。
        std::string profileError;
        application.applyTransferFrameRuntimeProfileEvent({.frameName = "offline_profile", .length = 3}, profileError);
        application.appendTransferRow({.direction = "TX", .bytes = {0xFA, 0x09, 0x0A}, .displayPosition = 5});
        for (int i = 0; i < 100 && application.historyRebuild_; ++i) application.pumpHistoryRebuild();
        const auto& timelineRows = application.dockStore_.receiveState().frameRows;
        if (timelineRows.size() != 2 || timelineRows.front().bytes != std::vector<std::uint8_t>({0xFA, 0x05, 0x06}) ||
            timelineRows.back().direction != "TX")
            throw std::runtime_error("timeline面板范围不能拿行数截断控制事件，也不能引入未来prefix，新增TX不得丢失");
        application.beginHistoryRebuild(config::TransferLogHistoryScope::RetainedCapture,
            application.historySwitchGeneration(), application.rawCaptureReplay_.eventIndex);
        for (int i = 0; i < 100 && application.historyRebuild_; ++i) application.pumpHistoryRebuild();
        if (application.historyRebuild_ || application.dockStore_.receiveState().frameRows.size() != 1 ||
            application.dockStore_.receiveState().frameRows.front().bytes != std::vector<std::uint8_t>({0xFA, 0x05, 0x06}))
            throw std::runtime_error("完整 timeline 范围只解析 eventIndex 前缀，不能包含未来 RX 或范围外 TX");
        application.rawCaptureReplay_.loaded = false;
        // 每行基线必须保留 set/clear 顺序，两个方向均不能用最新 profile 解释旧行。
        application.dockStore_.clearReceiveRows();
        application.transferProfiles_.reset();
        application.rememberTransferProfile({.type = plot::RawCaptureEventType::ProfileSet,
            .profile = {.frameName = frame.name, .length = 3}});
        application.appendHistoryReceiveRow({.direction = "RX", .bytes = {0xFA, 1, 2}});
        application.rememberTransferProfile({.type = plot::RawCaptureEventType::ProfileClear,
            .profile = {.frameName = frame.name}});
        application.appendHistoryReceiveRow({.direction = "TX", .bytes = {0xFA, 3, 4}});
        application.rememberTransferProfile({.type = plot::RawCaptureEventType::ProfileSet,
            .profile = {.frameName = frame.name, .length = 4}});
        application.appendHistoryReceiveRow({.direction = "TX", .bytes = {0xFA, 5, 6, 7}});
        application.beginHistoryRebuild(config::TransferLogHistoryScope::RetainedRows,
            application.historySwitchGeneration(), 3);
        for (int i = 0; i < 100 && application.historyRebuild_; ++i) application.pumpHistoryRebuild();
        const auto& profileRows = application.dockStore_.receiveState().frameRows;
        if (application.historyRebuild_ || profileRows.size() != 3 ||
            profileRows.front().bytes != std::vector<std::uint8_t>({0xFA, 1, 2}) ||
            !profileRows[1].message.empty() || profileRows[1].bytes != std::vector<std::uint8_t>({0xFA, 3, 4}) ||
            profileRows.back().bytes != std::vector<std::uint8_t>({0xFA, 5, 6, 7}) ||
            profileRows.back().message.find(frame.name) == std::string::npos)
            throw std::runtime_error("面板 profile set/clear 必须按接纳顺序作用于 RX/TX");
        // 无字节的尾部 clear 也属于上界 profile，必须交接到随后新数据。
        application.rememberTransferProfile({.type = plot::RawCaptureEventType::ProfileClear});
        application.beginHistoryRebuild(config::TransferLogHistoryScope::RetainedRows,
            application.historySwitchGeneration(), 3);
        for (int i = 0; i < 100 && application.historyRebuild_; ++i) application.pumpHistoryRebuild();
        const auto cleared = application.transferFrameParser_->rx.pushBytes({0xFA, 8, 9, 10});
        if (!cleared.frames.empty() || cleared.errors.empty())
            throw std::runtime_error("面板上界尾部 profile clear 必须交接，不能沿用最后一行 profile");
        // 追赶队列只保留元数据，来源裁剪必须取消而不是持有已淘汰字节。
        application.beginHistoryRebuild(config::TransferLogHistoryScope::RetainedRows,
            application.historySwitchGeneration(), 3);
        application.appendTransferRow({.direction = "RX", .bytes = std::vector<std::uint8_t>(4096, 0)});
        if (!application.historyRebuild_ || application.historyRebuild_->following.front().byteSize != 4096)
            throw std::runtime_error("追赶必须接纳新增来源位置和大小");
        application.dockStore_.receiveState().rows.pop_front();
        application.pumpHistoryRebuild();
        if (application.historyRebuild_) throw std::runtime_error("面板持续裁剪必须有界取消旧任务");
        // 超追赶预算取消任务，但当前新帧仍由原 UI parser 接纳并能提交。
        frame.runtimeProfile = false; frame.size = 3;
        scripting::FrameStreamParser pressure(scripting::StreamBufferDefinition{.capacity = 4096}, {frame});
        application.transferFrameParser_ = Application::TransferFrameParserState{.rx = pressure, .tx = pressure};
        application.dockStore_.receiveState().displayMode = dock::TransferLogDisplayMode::ParsedFrames;
        const auto oldByteLimit = application.runtimeConfig_.gui.rawCapture.liveLimitBytes;
        application.runtimeConfig_.gui.rawCapture.liveLimitBytes = 2;
        application.beginHistoryRebuild(config::TransferLogHistoryScope::RetainedRows,
            application.historySwitchGeneration(), application.dockStore_.receiveState().rows.size());
        application.appendTransferRow({.direction = "RX", .bytes = {0xFA, 11, 12}});
        if (application.historyRebuild_) throw std::runtime_error("超预算追赶必须显式取消");
        application.flushPendingTransferFrameRows(256);
        if (application.dockStore_.receiveState().frameRows.back().bytes != std::vector<std::uint8_t>({0xFA, 11, 12}))
            throw std::runtime_error("取消追赶不能丢失当前新帧");
        application.runtimeConfig_.gui.rawCapture.liveLimitBytes = oldByteLimit;
        // 来源裁剪取消任务，不持有或访问旧 vector 引用。
        capture.events = {{.bytes = std::vector<std::uint8_t>(256 * 1024, 0), .displayPosition = 10}};
        application.beginHistoryRebuild(config::TransferLogHistoryScope::RetainedCapture,
                                        application.historySwitchGeneration(), 1);
        capture.events.front().displayPosition = 11;
        application.pumpHistoryRebuild();
        if (application.historyRebuild_) throw std::runtime_error("来源裁剪必须取消旧任务");
    }
    static void verifySkipPreservesFramesAndProfile(Application& application, bool automatic)
    {
        scripting::StreamFrameDefinition frame;
        frame.name = "offline_profile";
        frame.header = {0xFA};
        frame.runtimeProfile = true;
        scripting::FrameStreamParser parser(scripting::StreamBufferDefinition{.capacity = 64}, {frame});
        std::string error;
        if (!parser.setRuntimeProfile(frame.name, scripting::StreamRuntimeProfile{.length = 3}, error))
            throw std::runtime_error(error);
        parser.pushBytes({0xFA, 0x01}); // 切换前半帧必须丢弃，但 profile 要保留。
        application.transferFrameParser_ = Application::TransferFrameParserState{.rx = parser, .tx = parser};
        auto& receive = application.dockStore_.receiveState();
        receive.displayMode = dock::TransferLogDisplayMode::RawChunks;
        application.dockStore_.appendTransferFrameRows({{.direction = "RX", .bytes = {0x10}, .message = "existing"}});
        application.pendingTransferFrameRows_.push_back({.direction = "TX", .bytes = {0x20}, .message = "pending"});
        application.runtimeConfig_.gui.historyOnParsedSwitch = automatic
            ? config::TransferLogHistoryOnParsedSwitch::SkipHistory : config::TransferLogHistoryOnParsedSwitch::Ask;
        application.requestParsedHistorySwitch();
        if (!automatic) {
            application.cancelHistorySwitch();
            if (receive.displayMode != dock::TransferLogDisplayMode::RawChunks ||
                receive.frameRows.back().message != "existing" || application.pendingTransferFrameRows_.back().message != "pending")
                throw std::runtime_error("取消必须保留模式、缓存与完整 pending 帧");
            application.requestParsedHistorySwitch();
            application.resolveHistorySwitch(config::TransferLogHistoryOnParsedSwitch::SkipHistory);
        }
        if (receive.displayMode != dock::TransferLogDisplayMode::ParsedFrames ||
            receive.frameRows.back().message != "existing" || application.pendingTransferFrameRows_.back().message != "pending")
            throw std::runtime_error("不使用历史必须保留已有帧和完整 pending 帧");
        auto& state = *application.transferFrameParser_;
        for (auto* direction : {&state.rx, &state.tx}) {
            if (!direction->pushBytes({0x02}).frames.empty())
                throw std::runtime_error("切换前半帧不得与切换后尾字节拼接");
            const auto parsed = direction->pushBytes({0xFA, 0x03, 0x04});
            if (parsed.frames.size() != 1 || parsed.frames.front().raw != std::vector<std::uint8_t>({0xFA, 0x03, 0x04}))
                throw std::runtime_error("拒绝必须保留离线 schema 和 runtime profile，之后新帧照常解析");
        }
        application.flushPendingTransferFrameRows(1);
        if (receive.frameRows.back().message != "pending")
            throw std::runtime_error("已生成完整 pending 帧必须仍可提交");
    }
};
}

static void waitFor(app::Application& app, const std::function<bool()>& ready, bool pump = true)
{
    const auto deadline = Clock::now() + std::chrono::seconds(40);
    while (!ready()) {
        require(Clock::now() < deadline, "task timed out");
        if (pump) app.pumpOnce();
        const auto status = app.dataTransferStatus();
        require(!status.complete || status.canceled || status.error.empty(), status.error);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// 用户主诉回归：导入含原始字节的记录后，收发面板必须默认原始模式显示原始 RX/TX，
// 且带 TX 状态的行不得遮蔽原始字节（显示/复制/单行导出路径都要保护字节与状态共存）。
static void testImportedRawRecordsVisibleAndTxBytesPreserved()
{
    app::Application application;
    require(application.initialize(), "导入可见性回归初始化失败");
    // 面板此前停在逐帧视图：导入后必须被切回原始模式，否则原始行不可见。
    application.docks().receiveState().displayMode = dock::TransferLogDisplayMode::ParsedFrames;

    plot::RawCaptureFileData data;
    data.source = "uart0";
    data.capturedAtMs = 1000;
    // 顺序：RX 2 字节 -> TX 3 字节（带状态，端点为空须回退 data.source）-> RX 1 字节。
    data.events = {
        {.type = plot::RawCaptureEventType::RxBytes, .timestampMs = 1001, .bytes = {0x01, 0x02}, .endpoint = "uart0"},
        {.type = plot::RawCaptureEventType::TxBytes, .timestampMs = 1002, .bytes = {0x0A, 0x0B, 0x0C},
         .writeStatus = "写入失败"},
        {.type = plot::RawCaptureEventType::RxBytes, .timestampMs = 1003, .bytes = {0x03}, .endpoint = "uart0"},
    };
    std::string error;
    require(application.importRawRecords(data, error), error);

    // 1. 导入后默认使用原始模式。
    require(application.docks().receiveState().displayMode == dock::TransferLogDisplayMode::RawChunks,
            "导入含原始字节记录后必须使用原始模式");
    const auto& rows = application.docks().receiveState().rows;
    require(rows.size() == 3, "导入必须生成全部 RX/TX 原始行");

    // 2. 字节、顺序、时间戳、端点与状态全部保留。
    require(rows[0].bytes == std::vector<std::uint8_t>({0x01, 0x02}) && rows[0].timestampMs == 1001 &&
                rows[0].direction == "RX" && rows[0].endpoint == "uart0",
            "第一行 RX 字节/顺序/时间戳/端点必须保留");
    require(rows[1].bytes == std::vector<std::uint8_t>({0x0A, 0x0B, 0x0C}) && rows[1].timestampMs == 1002 &&
                rows[1].direction == "TX" && rows[1].message == "写入失败",
            "TX 行字节与状态必须同时保留");
    require(rows[2].bytes == std::vector<std::uint8_t>({0x03}) && rows[2].timestampMs == 1003,
            "第三行 RX 字节/时间戳必须保留");
    require(rows[1].endpoint == "uart0", "TX 事件端点为空时必须回退 data.source");

    // 3. TX 状态不得遮蔽原始字节：内容/单行/整段导出都必须同时带状态与字节。
    const auto content = dock::formatReceiveRowContent(rows[1], true);
    require(content.find("写入失败") != std::string::npos && content.find("0A") != std::string::npos,
            "带状态 TX 行的内容格式化不得遮蔽原始字节");
    const auto singleLine = dock::formatReceiveRowSingleLine(rows[1], true, true);
    require(singleLine.find("TX") != std::string::npos && singleLine.find("写入失败") != std::string::npos &&
                singleLine.find("0A") != std::string::npos,
            "带状态 TX 行的单行导出不得遮蔽原始字节");
    std::vector<dock::ReceiveRow> rowCopy(rows.begin(), rows.end());
    const auto exported = dock::formatReceiveRowsText(rowCopy, true, true);
    require(exported.find("写入失败") != std::string::npos && exported.find("0A") != std::string::npos &&
                exported.find("01") != std::string::npos && exported.find("03") != std::string::npos,
            "整段导出必须同时保留 TX 状态与全部原始字节");
    // 纯字节行（无状态）保持原有格式。
    require(dock::formatReceiveRowContent(rows[0], true).find("01") != std::string::npos,
            "无状态 RX 行按字节格式化");

    application.shutdown();
}

// 三态历史切换回归：ask 询问/接受/拒绝/取消、use_history 按范围重建替换、skip_history 只清半帧、
// 来源代次失效（seek/重新导入）。覆盖面板行与 timeline 事件前缀两种范围。
static void testParsedHistorySwitchTriState()
{
    using protoscope::config::TransferLogHistoryOnParsedSwitch;
    using protoscope::config::TransferLogHistoryScope;
    app::Application application;
    require(application.initialize(), "三态历史切换回归初始化失败");
    const auto protocol = std::filesystem::path(__FILE__).parent_path() / "fixtures/protocols/stream_frame_only";
    require(application.reloadProtocolDirectory(protocol.generic_string(), true), "stream fixture 加载失败");

    // 载入含 RX 原始字节的 timeline（BrowseRaw），推进前缀使历史可选。
    plot::RawCaptureFileData capture;
    capture.source = "probe";
    // stream_sample 帧：AA 55 len(=0) value crc_lo crc_hi；CRC 校验失败也生成帧行，仍验证重建路径。
    capture.events = {
        {.type = plot::RawCaptureEventType::RxBytes, .timestampMs = 10, .bytes = {0xAA, 0x55, 0x00, 0x07, 0x00, 0x00}, .endpoint = "probe"},
        {.type = plot::RawCaptureEventType::RxBytes, .timestampMs = 20, .bytes = {0xAA, 0x55, 0x00, 0x08, 0x00, 0x00}, .endpoint = "probe"},
        {.type = plot::RawCaptureEventType::RxBytes, .timestampMs = 30, .bytes = {0xAA, 0x55, 0x00, 0x09, 0x00, 0x00}, .endpoint = "probe"},
    };
    std::string error;
    require(application.loadRawCaptureReplayTimeline(capture, error), error);
    // 推进到前缀 eventIndex=2（两个事件），第三个是未来数据不得进入历史。
    require(application.seekRawCaptureReplay(2, error), error);
    for (int i = 0; i < 200 && application.rawCaptureReplayStatus().eventIndex < 2; ++i) application.pumpOnce();
    require(application.rawCaptureReplayStatus().eventIndex == 2, "前缀推进到 eventIndex=2 失败");
    const auto generationBefore = application.rawCaptureReplayStatus().eventCount > 0
                                      ? application.docks().receiveState().rows.size()
                                      : 0;

    // 默认 ask：有历史时弹确认，确认前保持原始模式。
    require(application.historySwitchStrategy() == TransferLogHistoryOnParsedSwitch::Ask, "缺省必须是 ask");
    application.requestParsedHistorySwitch();
    require(application.historySwitchConfirmationPending(), "ask 策略必须有历史时打开确认");
    require(application.docks().receiveState().displayMode == dock::TransferLogDisplayMode::RawChunks,
            "确认前必须保持原始模式");

    // 取消（Escape 等同）：保持原始模式，关闭 modal。
    application.cancelHistorySwitch();
    require(!application.historySwitchConfirmationPending() &&
                application.docks().receiveState().displayMode == dock::TransferLogDisplayMode::RawChunks,
            "取消后必须保持原始模式");

    // 再次请求并拒绝（不使用历史）：切到逐帧但不重建（保留既有 frameRows 为空）。
    const auto framesBeforeReject = application.docks().receiveState().frameRows.size();
    application.requestParsedHistorySwitch();
    require(application.historySwitchConfirmationPending(), "再次请求应重新打开确认");
    application.resolveHistorySwitch(TransferLogHistoryOnParsedSwitch::SkipHistory);
    require(!application.historySwitchConfirmationPending(), "拒绝后 modal 必须关闭");
    require(application.docks().receiveState().displayMode == dock::TransferLogDisplayMode::ParsedFrames,
            "拒绝后切到逐帧视图");
    require(application.docks().receiveState().frameRows.size() == framesBeforeReject,
            "不使用历史不得重建或删除已有帧结果");

    // 自动与询问拒绝均保留完整帧/pending、清半帧但保留离线 schema/profile。
    application.requestParsedHistorySwitch(); // 切回原始
    app::ApplicationBackpressureTestAccess::verifySkipPreservesFramesAndProfile(application, true);
    application.requestParsedHistorySwitch();
    app::ApplicationBackpressureTestAccess::verifySkipPreservesFramesAndProfile(application, false);
    application.docks().clearTransferFrameRows();

    // 切回原始使任务/旧确认失效。
    application.requestParsedHistorySwitch();
    require(!application.historySwitchConfirmationPending() &&
                application.docks().receiveState().displayMode == dock::TransferLogDisplayMode::RawChunks,
            "切回原始应直接切换且失效旧确认");

    // 前面的拒绝隔离测试显式注入了另一离线 schema；恢复当前 BrowseRaw 来源 schema。
    app::ApplicationBackpressureTestAccess::restoreBrowseSchema(application);
    // 询问接受（使用历史）：timeline 范围只允许 [0,eventIndex) 前缀，不提前显示未来数据。
    application.requestParsedHistorySwitch();
    application.resolveHistorySwitch(TransferLogHistoryOnParsedSwitch::UseHistory);
    for (int i = 0; i < 50 && application.docks().receiveState().frameRows.empty(); ++i) application.pumpOnce();
    const auto& frameRows = application.docks().receiveState().frameRows;
    require(!frameRows.empty(), "使用历史必须重建出帧结果");
    for (const auto& row : frameRows) {
        require(row.timestampMs == 10 || row.timestampMs == 20, "未来事件（timestampMs=30）不得进入历史帧结果");
    }
    static_cast<void>(generationBefore);

    // seek 使任务/旧确认失效：过期结果不得提交到新来源；逐帧行被清且视图回原始。
    require(application.seekRawCaptureReplay(1, error), error);
    require(application.docks().receiveState().frameRows.empty() &&
                application.docks().receiveState().displayMode == dock::TransferLogDisplayMode::RawChunks,
            "seek 后必须清过期帧结果并回原始视图");

    application.shutdown();

    // 面板行范围（retained_rows）：纯导入无 timeline，使用历史按面板行重建。
    app::Application app2;
    require(app2.initialize(), "面板行范围回归初始化失败");
    require(app2.reloadProtocolDirectory(protocol.generic_string(), true), "stream fixture 二次加载失败");
    plot::RawCaptureFileData data;
    data.source = "uart0";
    data.events = {
        {.type = plot::RawCaptureEventType::RxBytes, .timestampMs = 1, .bytes = {0xAA, 0x55, 0x00, 0x07, 0x00, 0x00}, .endpoint = "uart0"},
    };
    require(app2.importRawRecords(data, error), error);
    require(app2.docks().receiveState().displayMode == dock::TransferLogDisplayMode::RawChunks, "导入后默认原始模式");
    // 自动 use_history 策略。
    config::AppConfig strategyConfig;
    strategyConfig.gui.historyOnParsedSwitch = TransferLogHistoryOnParsedSwitch::UseHistory;
    strategyConfig.gui.historyScope = TransferLogHistoryScope::RetainedRows;
    app2.applyConfig(strategyConfig);
    app2.requestParsedHistorySwitch();
    for (int i = 0; i < 50 && app2.docks().receiveState().frameRows.empty(); ++i) app2.pumpOnce();
    require(!app2.docks().receiveState().frameRows.empty(), "retained_rows 范围必须按面板行重建帧结果");

    // 同事件数再导入必须让旧确认/旧任务失效：旧帧结果不得残留。
    app2.requestParsedHistorySwitch();
    require(app2.historySwitchConfirmationPending() ||
                app2.docks().receiveState().displayMode == dock::TransferLogDisplayMode::RawChunks,
            "重建完成后再请求应回到可询问/原始状态");
    require(app2.importRawRecords(data, error), error);
    require(!app2.historySwitchConfirmationPending(), "再导入必须使旧确认失效");
    require(app2.docks().receiveState().frameRows.empty(), "再导入必须清旧逐帧结果");
    require(app2.docks().receiveState().displayMode == dock::TransferLogDisplayMode::RawChunks, "再导入后默认原始模式");

    // 清空入口：使旧确认失效并清原始行与逐帧展示。
    // 恢复 ask 策略使确认 modal 可弹出（applyConfig 已把运行时策略持久为 use_history）。
    config::AppConfig askConfig;
    askConfig.gui.historyOnParsedSwitch = TransferLogHistoryOnParsedSwitch::Ask;
    askConfig.gui.historyScope = TransferLogHistoryScope::RetainedRows;
    app2.applyConfig(askConfig);
    app2.requestParsedHistorySwitch();
    require(app2.historySwitchConfirmationPending(), "清空前应可打开确认");
    app2.clearTransferLogHistory();
    require(!app2.historySwitchConfirmationPending(), "清空必须使旧确认失效");
    require(app2.docks().receiveState().rows.empty() && app2.docks().receiveState().frameRows.empty(),
            "清空必须同时清原始行与逐帧展示");
    app2.shutdown();
}

static void testProtocolReloadDefersLargeHistory()
{
    app::Application application;
    require(application.initialize(), "协议重载预算回归初始化失败");
    const auto protocol = std::filesystem::path(__FILE__).parent_path() / "fixtures/protocols/stream_frame_only";
    require(application.reloadProtocolDirectory(protocol.generic_string(), true), "初始协议加载失败");
    plot::RawCaptureFileData capture;
    capture.source = "reload-budget";
    capture.events = {{.type = plot::RawCaptureEventType::RxBytes,
        .bytes = std::vector<std::uint8_t>(256 * 1024, 0)}};
    // fixture 的 len 为 payload 长度，value 占一字节；CRC 覆盖头、长度和 value。
    std::vector<std::uint8_t> frame{0xAA, 0x55, 0x01, 0x07};
    const auto crc = protocol_utils::crc16Modbus(frame);
    frame.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
    frame.push_back(static_cast<std::uint8_t>((crc >> 8U) & 0xFFU));
    capture.events.front().bytes.insert(capture.events.front().bytes.end(), frame.begin(), frame.end());
    std::string error;
    require(application.importRawRecords(capture, error), error);
    application.docks().receiveState().displayMode = dock::TransferLogDisplayMode::ParsedFrames;
    // 真实入口：成功重新加载协议触发 refreshTransferFrameDisplayAfterProtocolReload。
    require(application.reloadProtocolDirectory(protocol.generic_string(), true), "重载协议失败");
    app::ApplicationBackpressureTestAccess::verifyReloadHistoryDeferred(application, true);
    application.pumpOnce();
    app::ApplicationBackpressureTestAccess::verifyReloadHistoryDeferred(application, false);
    // 历史解析完成只交接 pending；噪声行与末尾合法帧仍按既有主循环预算分批提交。
    waitFor(application, [&] {
        return !app::ApplicationBackpressureTestAccess::historyActive(application) &&
               !app::ApplicationBackpressureTestAccess::historyOutputPending(application);
    });
    const auto& rows = application.docks().receiveState().frameRows;
    const auto parsedCount = std::count_if(rows.begin(), rows.end(), [&](const auto& row) {
        return row.bytes == frame && !row.message.empty();
    });
    require(!rows.empty() && rows.back().bytes == frame && parsedCount == 1,
        "协议重载分批完成后必须替换出唯一匹配历史帧，不丢不重；噪声仍允许错误展示行");
    application.shutdown();
}

static void testPackageHistoryUsesOfflineSchema()
{
    app::Application application;
    require(application.initialize(), "包内历史回归初始化失败");
    const auto live = std::filesystem::path(__FILE__).parent_path() / "fixtures/protocols/stream_frame_only";
    require(application.reloadProtocolDirectory(live.generic_string(), true), "实时 schema 加载失败");
    const std::string script = R"lua(
function stream()
  return { buffer = { capacity = 64, overflow = "drop_oldest" },
    frames = { { name = "package_history", header = { 0xFA }, size = 3,
      on_frame = function(ctx, frame) proto.emit("history_callback", frame.name) end } } }
end
)lua";
    session::SessionPackageData package;
    package.entries.push_back({.name = "protocol/main.lua", .bytes = {script.begin(), script.end()}});
    plot::RawCaptureFileData capture;
    capture.events = {{.type = plot::RawCaptureEventType::RxBytes, .timestampMs = 10, .bytes = {0xFA, 1, 2}},
                      {.type = plot::RawCaptureEventType::RxBytes, .timestampMs = 20, .bytes = {0xFA, 3, 4}}};
    std::string error;
    require(application.loadRawCaptureReplayTimeline(capture, app::Application::OfflineReplayProcessing::PackageProtocol,
        app::Application::OfflineReplayPacing::FastBatchParse, error, &package), error);
    const auto waitForReplayPrefix = [&] {
        waitFor(application, [&] {
            const auto status = application.rawCaptureReplayStatus();
            require(status.error.empty(), status.error);
            require(status.phase != app::Application::OfflineReplayPhase::Failed, "离线回放不得进入失败态");
            // inputBacklog 是未来时间轴事件数，不能要求未回放的后缀为零。
            return status.phase == app::Application::OfflineReplayPhase::Paused && status.eventIndex == 1 &&
                   status.inputBacklog == status.eventCount - status.eventIndex &&
                   status.workerBacklog == 0 && status.outputBacklog == 0;
        });
    };
    require(application.seekRawCaptureReplay(1, error), error);
    waitForReplayPrefix();
    // 真实流回调必须已执行，不能用没有回调的空日志基线证明历史非重入。
    const auto& scriptLog = application.docks().scriptState().rows;
    const auto callbackRows = std::count_if(scriptLog.begin(), scriptLog.end(), [](const auto& row) {
        return row.message.find("history_callback") != std::string::npos;
    });
    require(callbackRows == 1, "包内协议已回放前缀必须恰好执行一次真实流回调");
    const auto scriptRows = scriptLog.size();
    const auto samples = application.docks().waveState().buffer.snapshot(-1e9, 1e9, false);
    application.requestParsedHistorySwitch();
    require(application.historySwitchConfirmationPending(), "包内前缀必须询问历史");
    application.resolveHistorySwitch(config::TransferLogHistoryOnParsedSwitch::UseHistory);
    waitFor(application, [&] { return !app::ApplicationBackpressureTestAccess::historyActive(application); });
    const auto& rows = application.docks().receiveState().frameRows;
    require(rows.size() == 1 && rows.front().bytes == std::vector<std::uint8_t>({0xFA, 1, 2}) &&
        rows.front().message.find("package_history") != std::string::npos,
        "真实 PackageProtocol 历史必须用包内 schema，只解析已回放前缀");
    const auto& afterScriptLog = application.docks().scriptState().rows;
    require(afterScriptLog.size() == scriptRows &&
        std::count_if(afterScriptLog.begin(), afterScriptLog.end(), [](const auto& row) {
            return row.message.find("history_callback") != std::string::npos;
        }) == callbackRows, "独立 UI 历史不得重入 Lua callback");
    const auto after = application.docks().waveState().buffer.snapshot(-1e9, 1e9, false);
    require(after.channels.size() == samples.channels.size(), "UI 历史不得修改波形通道");
    for (std::size_t i = 0; i < samples.channels.size(); ++i)
        require(after.channels[i].totalSamples == samples.channels[i].totalSamples, "UI 历史不得修改波形样本");
    // CurrentProtocol 也走真实离线加载，切换后的 UI 历史不使用上一个包内 schema。
    application.unloadRawCaptureReplayTimeline();
    std::vector<std::uint8_t> currentFrame{0xAA, 0x55, 0x01, 0x07};
    const auto currentCrc = protocol_utils::crc16Modbus(currentFrame);
    currentFrame.push_back(static_cast<std::uint8_t>(currentCrc & 0xFFU));
    currentFrame.push_back(static_cast<std::uint8_t>((currentCrc >> 8U) & 0xFFU));
    capture.events = {{.type = plot::RawCaptureEventType::RxBytes, .timestampMs = 30,
                       .bytes = currentFrame},
                      {.type = plot::RawCaptureEventType::RxBytes, .timestampMs = 40,
                       .bytes = currentFrame}};
    require(application.loadRawCaptureReplayTimeline(capture, app::Application::OfflineReplayProcessing::CurrentProtocol,
        app::Application::OfflineReplayPacing::FastBatchParse, error), error);
    require(application.seekRawCaptureReplay(1, error), error);
    waitForReplayPrefix();
    require(application.rawCaptureReplayStatus().hasStreamSchema, "CurrentProtocol 必须实际加载离线 stream schema");
    const auto& currentScriptLog = application.docks().scriptState().rows;
    require(std::any_of(currentScriptLog.begin(), currentScriptLog.end(), [](const auto& row) {
        return row.message.find("stream_frame") != std::string::npos;
    }), "CurrentProtocol 已回放前缀必须执行真实 stream 回调");
    const auto currentScriptRows = currentScriptLog.size();
    const auto currentSamples = application.docks().waveState().buffer.snapshot(-1e9, 1e9, false);
    application.requestParsedHistorySwitch();
    require(application.historySwitchConfirmationPending(), "CurrentProtocol 前缀必须询问");
    application.resolveHistorySwitch(config::TransferLogHistoryOnParsedSwitch::UseHistory);
    waitFor(application, [&] { return !app::ApplicationBackpressureTestAccess::historyActive(application); });
    require(application.docks().receiveState().frameRows.size() == 1 &&
        application.docks().receiveState().frameRows.front().bytes == capture.events.front().bytes &&
        application.docks().receiveState().frameRows.front().message.find("stream_sample") != std::string::npos,
        "真实 CurrentProtocol 历史必须使用其离线 schema，且只解析已回放前缀");
    require(application.docks().scriptState().rows.size() == currentScriptRows,
        "CurrentProtocol 的 UI 历史不得重入 Lua callback");
    const auto currentAfter = application.docks().waveState().buffer.snapshot(-1e9, 1e9, false);
    require(currentAfter.channels.size() == currentSamples.channels.size(), "CurrentProtocol UI 历史不得修改波形通道");
    for (std::size_t i = 0; i < currentSamples.channels.size(); ++i)
        require(currentAfter.channels[i].totalSamples == currentSamples.channels[i].totalSamples,
            "CurrentProtocol UI 历史不得修改波形样本");
    application.shutdown();
}

static void testImportedSourceLabelsAfterReplay()
{
    app::Application application;
    require(application.initialize(), "来源名称回归初始化失败");
    const auto protocol = std::filesystem::path(__FILE__).parent_path() / "fixtures/protocols/import_source_label";
    require(application.reloadProtocolDirectory(protocol.generic_string(), true), "来源名称 fixture 加载失败");
    plot::RawCaptureFileData capture;
    capture.events = {
        {.type = plot::RawCaptureEventType::RxBytes, .bytes = {77}, .sequence = 1},
        {.type = plot::RawCaptureEventType::RxBytes, .bytes = {11, 12, 13}, .sequence = 2},
    };
    tests::ScopedTempPath file(tests::makeUniqueTempFile("protoscope-import-source-label", ".psraw"));
    std::string error;
    require(plot::writeRawCaptureFile(file.path(), capture, error), error);
    require(application.startDataImport(file.path(), error), error);
    waitFor(application, [&] { return application.dataTransferStatus().awaitingConfirmation; });
    application.confirmDataImport(true);
    waitFor(application, [&] {
        const auto status = application.dataTransferStatus();
        return status.complete && !status.active;
    });

    const auto assertWave = [&](const std::string& stage) {
        const auto& wave = application.docks().waveState();
        const auto snapshot = wave.buffer.snapshot(-1e9, 1e9, false);
        require(snapshot.channels.size() == 2 && wave.defaultChannelSpecs.size() == 2,
                stage + ": 内部解析必须建立两个通道及默认规格");
        const std::array<std::string, 2> labels{"raw-original", "raw-second"};
        for (std::size_t i = 0; i < labels.size(); ++i) {
            require(wave.buffer.channelSpec(i)->label == labels[i] &&
                        snapshot.channels[i].label == labels[i] && wave.defaultChannelSpecs[i].label == labels[i],
                    stage + ": Lua setup 不得改变导入的实际或默认名称（通道 " + std::to_string(i) + "）");
            require(snapshot.channels[i].totalSamples == 3,
                    stage + ": 内部重建必须保留三个样本，用户 setup/push 不得清空或追加");
            for (std::size_t j = 0; j < 3; ++j) {
                require(snapshot.channels[i].samples[j].time == static_cast<double>(j + 1) &&
                            snapshot.channels[i].samples[j].value == static_cast<double>(11 + j + i * 100),
                        stage + ": 内部解析样本值或时间不正确");
            }
        }
    };
    const auto tryLuaRename = [&](const std::string& stage) {
        assertWave(stage + "配置前");
        application.docks().scriptState().rows.clear();
        application.updateControlValue("rename", true);
        waitFor(application, [&] {
            require(application.docks().luaState().lastError.empty(), application.docks().luaState().lastError);
            const auto& rows = application.docks().scriptState().rows;
            return std::any_of(rows.begin(), rows.end(), [](const auto& row) {
                return row.message.find("source-label-rename-consumed") != std::string::npos;
            });
        });
        // 日志与真实 setup 同批消费后再核验，不能靠保护标记代替行为断言。
        assertWave(stage);
    };
    const auto waitCompleted = [&] {
        waitFor(application, [&] {
            const auto status = application.rawCaptureReplayStatus();
            require(status.error.empty(), status.error);
            return status.phase == app::Application::OfflineReplayPhase::Completed &&
                   status.inputBacklog == 0 && status.workerBacklog == 0 && status.outputBacklog == 0;
        });
    };

    tryLuaRename("首次重解析完成后");
    // 前缀中没有 setup，仅真实 Lua push(2) 隐式建立位置 0；不能用提前 setup 掩盖原名遗漏。
    require(application.seekRawCaptureReplay(1, error), error);
    waitFor(application, [&] {
        const auto status = application.rawCaptureReplayStatus();
        require(status.error.empty(), status.error);
        // inputBacklog 是尚未播放的时间轴事件数，不是 worker 待处理输入。
        return status.phase == app::Application::OfflineReplayPhase::Paused && status.eventIndex == 1 &&
               status.eventCount == 2 && status.inputBacklog == status.eventCount - status.eventIndex &&
               status.workerBacklog == 0 && status.outputBacklog == 0;
    });
    {
        const auto& wave = application.docks().waveState();
        const auto prefix = wave.buffer.snapshot(-1e9, 1e9, false);
        require(prefix.channels.size() == 2 && wave.defaultChannelSpecs.size() == 2,
                "高索引前缀必须隐式建立两个位置及默认规格");
        const std::array<std::string, 2> labels{"raw-original", "raw-second"};
        for (std::size_t i = 0; i < labels.size(); ++i)
            require(wave.buffer.channelSpec(i)->label == labels[i] && prefix.channels[i].label == labels[i] &&
                        wave.defaultChannelSpecs[i].label == labels[i],
                    "高索引前缀不得丢失冻结实际/default 原名（通道 " + std::to_string(i) + "）");
        require(prefix.channels[0].totalSamples == 0 && prefix.channels[1].totalSamples == 1 &&
                    prefix.channels[1].samples[0].time == 1 && prefix.channels[1].samples[0].value == 77,
                "无 setup 高索引前缀应只在第二通道产生准确样本");
    }
    require(application.playRawCaptureReplay(error), error);
    waitCompleted();
    tryLuaRename("短前缀继续完成后");
    for (int repeat = 0; repeat < 2; ++repeat) {
        require(application.seekRawCaptureReplay(application.rawCaptureReplayStatus().eventCount, error), error);
        waitCompleted();
        tryLuaRename("seek 完成后");
        require(application.playRawCaptureReplay(error), error);
        waitCompleted();
        tryLuaRename("完成后再次播放");
    }
    require(application.reloadProtocolDirectory(protocol.generic_string(), true), "导入后的普通 reload 失败");
    tryLuaRename("普通 reload 后");
    plot::WaveCsvData invalid;
    error.clear();
    require(!application.importWaveCsvData(invalid, error), "零通道导入应被拒绝");
    tryLuaRename("失败导入后");
    error.clear();
    require(application.startDataImport(file.path(), error), error);
    waitFor(application, [&] { return application.dataTransferStatus().awaitingConfirmation; });
    application.cancelDataTransfer();
    waitFor(application, [&] { return !application.dataTransferStatus().active; });
    tryLuaRename("确认前取消后");
    require(application.startDataImport(file.path(), error), error);
    waitFor(application, [&] { return application.dataTransferStatus().awaitingConfirmation; });
    application.confirmDataImport(true);
    waitFor(application, [&] {
        const auto status = application.dataTransferStatus();
        return status.complete && !status.active;
    });
    tryLuaRename("重新导入后");

    // 仅明确切换到实时来源后，同一 Lua label 配置才恢复普通写入行为。
    application.unloadRawCaptureReplayTimeline();
    application.setTransportFactoryForTest([](auto) { return std::make_unique<WriteProbe>(); });
    application.openTransport();
    application.pumpOnce();
    application.docks().scriptState().rows.clear();
    application.updateControlValue("rename", true);
    waitFor(application, [&] {
        const auto& rows = application.docks().scriptState().rows;
        return std::any_of(rows.begin(), rows.end(), [](const auto& row) {
            return row.message.find("source-label-rename-consumed") != std::string::npos;
        });
    });
    const auto& live = application.docks().waveState();
    require(live.buffer.channelSpec(0)->label == "lua-renamed" && live.defaultChannelSpecs[0].label == "lua-renamed" &&
                live.buffer.channelSpec(1)->label == "lua-second" && live.defaultChannelSpecs[1].label == "lua-second",
            "显式实时切换后 Lua label 必须恢复写入实际及默认名称");
    const auto snapshot = live.buffer.snapshot(-1e9, 1e9, false);
    require(snapshot.channels[0].totalSamples == 1 && snapshot.channels[0].samples[0].value == 999,
            "显式实时切换后 Lua push 必须恢复");
    application.shutdown();
}

static void testImportedLuaBitToggle()
{
    app::Application application;
    require(application.initialize(), "Bit test application initialization failed");
    const auto protocol = std::filesystem::path(__FILE__).parent_path() / "fixtures/protocols/import_bit_toggle";
    require(application.reloadProtocolDirectory(protocol.generic_string(), true), "Bit fixture did not load");
    plot::WaveCsvData data;
    data.source = "protected-import";
    data.timeAxis = "frequency";
    data.sampleFrequencyHz = 10;
    data.channels.resize(2);
    for (std::size_t i = 0; i < data.channels.size(); ++i) {
        auto& channel = data.channels[i];
        channel.label = "import-" + std::to_string(i);
        channel.unit = "V";
        channel.spec = {.ratio = 2, .scale = 3, .offset = 4,
                        .color = std::array<float, 4>{0.2F, 0.4F, 0.6F, 1.0F}, .lineWidth = 2.0F,
                        .bitDisplay = {.firstBit = 3, .bitCount = 5, .yOffset = 7, .hoverReadout = false}};
        channel.samples = {{1, 17}, {2, 18}, {3, 19}};
        channel.sampleIndexOffset = 42;
    }
    std::string error;
    require(application.importWaveCsvData(data, error), error);
    auto& wave = application.docks().waveState();
    wave.view.initialized = true;
    wave.view.defaultViewportPending = false;
    wave.view.viewMinTime = 1.5;
    wave.view.viewMaxTime = 2.5;
    const auto originalConfig = wave.buffer.viewConfig();
    const auto originalEpoch = wave.buffer.historyEpoch();
    const auto originalEvents = wave.rawCapture.events.size();
    // 两组名称刻意不同，不能把实际名称整份回写默认基线。
    for (std::size_t i = 0; i < 2; ++i)
        wave.defaultChannelSpecs[i].label = "default-import-" + std::to_string(i);
    std::array<plot::ChannelSpec, 2> expectedSpecs;
    for (std::size_t i = 0; i < expectedSpecs.size(); ++i) {
        expectedSpecs[i] = data.channels[i].spec;
        expectedSpecs[i].label = data.channels[i].label;
        expectedSpecs[i].unit = data.channels[i].unit;
    }
    const auto assertDisplaySpec = [](const auto& actual, const plot::ChannelSpec& expected,
                                      const std::string& stage) {
        require(actual.label == expected.label && actual.unit == expected.unit &&
                    actual.ratio == expected.ratio && actual.scale == expected.scale && actual.offset == expected.offset &&
                    actual.color == expected.color && actual.lineWidth == expected.lineWidth &&
                    actual.bitDisplay == expected.bitDisplay,
                stage + ": Lua 非名称显示规格或原名不正确");
    };
    // 连续相同 setup 也不能解除导入保护或新增通道；短数组必须保留尾部最新规格。
    for (const std::string id : {"enable", "enable", "disable", "disable", "enable", "omit", "omit"}) {
        application.updateControlValue(id, true);
        waitFor(application, [&] {
            require(application.docks().luaState().lastError.empty(), application.docks().luaState().lastError);
            const auto& rows = application.docks().scriptState().rows;
            return std::any_of(rows.begin(), rows.end(), [&](const auto& row) {
                return row.message.find("bit-toggle-" + id) != std::string::npos;
            });
        });
        // 日志与 setup 在同一输出批次应用；清除日志保证下一次同名控件等待的是新回调。
        application.docks().scriptState().rows.clear();
        // 首先核验具体非名称字段，旧 enabled-only 实现必须在这里产生行为红测。
        require(wave.buffer.channelSpec(0)->bitDisplay.firstBit == (id == "omit" ? 0U : 6U),
                "导入Lua first_bit未生效");
        require(wave.buffer.channelSpec(0)->bitDisplay.enabled == (id == "enable"), "Lua Bit toggle not applied");
        require(wave.buffer.channelSpec(1)->bitDisplay.enabled, "unmentioned imported channel changed");
        expectedSpecs[0] = id == "omit" ? plot::ChannelSpec{} : plot::ChannelSpec{
            .unit = "changed", .ratio = 99, .scale = 88, .offset = 77,
            .color = std::array<float, 4>{1, 0, 0, 1}, .lineWidth = 4.0F,
            .bitDisplay = {.enabled = id == "enable", .firstBit = 6, .bitCount = 3,
                           .yOffset = 9, .hoverReadout = true}};
        expectedSpecs[0].label = data.channels[0].label;
        if (id == "enable") {
            expectedSpecs[1] = plot::ChannelSpec{
                .label = data.channels[1].label, .unit = "flags", .ratio = 5, .scale = 6, .offset = -7,
                .color = std::array<float, 4>{0, 1, 0, 1}, .lineWidth = 3.5F,
                .bitDisplay = {.enabled = true, .firstBit = 11, .bitCount = 2,
                               .yOffset = -3, .hoverReadout = false}};
        }
        const auto snapshot = wave.buffer.snapshot(-1e9, 1e9, false);
        require(snapshot.channels.size() == 2 && wave.defaultChannelSpecs.size() == 2,
                "Lua changed imported channel count");
        require(snapshot.source == data.source, "Lua changed imported source");
        for (std::size_t i = 0; i < 2; ++i) {
            const auto& expected = expectedSpecs[i];
            assertDisplaySpec(*wave.buffer.channelSpec(i), expected, "actual");
            auto expectedDefault = expected;
            expectedDefault.label = "default-import-" + std::to_string(i);
            assertDisplaySpec(wave.defaultChannelSpecs[i], expectedDefault, "default");
            assertDisplaySpec(snapshot.channels[i], expected, "snapshot");
            require(snapshot.channels[i].totalSamples == 3 && snapshot.channels[i].sampleIndexOffset == 42,
                    "Lua reset history or appended samples");
            for (std::size_t j = 0; j < 3; ++j)
                require(snapshot.channels[i].samples[j].time == data.channels[i].samples[j].time &&
                        snapshot.channels[i].samples[j].value == data.channels[i].samples[j].value,
                        "Lua changed imported sample");
        }
        require(wave.buffer.historyEpoch() == originalEpoch && wave.buffer.importedLabelsReadOnly(),
                "Lua reset imported history protection");
        require(wave.buffer.viewConfig().timeScale == originalConfig.timeScale &&
                wave.buffer.viewConfig().timeUnit == originalConfig.timeUnit &&
                wave.buffer.viewConfig().historyLimit == originalConfig.historyLimit &&
                wave.buffer.viewConfig().verticalMin == originalConfig.verticalMin &&
                wave.buffer.viewConfig().verticalMax == originalConfig.verticalMax &&
                wave.buffer.viewConfig().verticalUnit == originalConfig.verticalUnit &&
                wave.buffer.viewConfig().displayFormula == originalConfig.displayFormula,
                "Lua changed imported view config");
        require(wave.view.initialized && !wave.view.defaultViewportPending &&
                wave.view.viewMinTime == 1.5 && wave.view.viewMaxTime == 2.5, "Lua reset viewport");
        require(wave.view.sampleFrequencyHz == 10 &&
                wave.view.timeAxisSource == plot::WaveTimeAxisSource::SampleFrequency,
                "Lua changed imported time axis");
        require(wave.rawCapture.events.size() == originalEvents, "unapplied Lua setup recorded in raw capture");
    }

    const auto sendControl = [&](const std::string& id) {
        application.docks().scriptState().rows.clear();
        application.updateControlValue(id, true);
        waitFor(application, [&] {
            const auto& rows = application.docks().scriptState().rows;
            return std::any_of(rows.begin(), rows.end(), [&](const auto& row) {
                return row.message.find("bit-toggle-" + id) != std::string::npos;
            });
        });
    };
    const auto assertProtectedData = [&] {
        const auto snapshot = wave.buffer.snapshot(-1e9, 1e9, false);
        require(snapshot.source == data.source && snapshot.channels.size() == 2 &&
                    wave.defaultChannelSpecs.size() == 2 && wave.buffer.historyEpoch() == originalEpoch &&
                    wave.buffer.importedLabelsReadOnly(), "显示覆盖或恢复改变了导入结构");
        require(wave.buffer.viewConfig().timeScale == originalConfig.timeScale &&
                    wave.buffer.viewConfig().timeUnit == originalConfig.timeUnit &&
                    wave.buffer.viewConfig().historyLimit == originalConfig.historyLimit &&
                    wave.buffer.viewConfig().verticalMin == originalConfig.verticalMin &&
                    wave.buffer.viewConfig().verticalMax == originalConfig.verticalMax &&
                    wave.buffer.viewConfig().verticalUnit == originalConfig.verticalUnit &&
                    wave.buffer.viewConfig().displayFormula == originalConfig.displayFormula &&
                    wave.rawCapture.events.size() == originalEvents,
                "显示覆盖或恢复改变了视窗配置或原始事件");
        for (std::size_t i = 0; i < 2; ++i) {
            require(snapshot.channels[i].label == data.channels[i].label &&
                        wave.buffer.channelSpec(i)->label == data.channels[i].label &&
                        wave.defaultChannelSpecs[i].label == "default-import-" + std::to_string(i) &&
                        snapshot.channels[i].totalSamples == 3 && snapshot.channels[i].sampleIndexOffset == 42,
                    "显示覆盖或恢复改变了原名或样本数");
            for (std::size_t j = 0; j < 3; ++j)
                require(snapshot.channels[i].samples[j].time == data.channels[i].samples[j].time &&
                            snapshot.channels[i].samples[j].value == data.channels[i].samples[j].value,
                        "显示覆盖或恢复改变了准确样本");
        }
    };
    wave.channelOverrides.resize(2);
    auto& overrides = wave.channelOverrides[0];
    overrides.labelOverridden = true;
    overrides.label = "forbidden-user-label";
    overrides.ratioOverridden = overrides.scaleOverridden = overrides.offsetOverridden = true;
    overrides.colorOverridden = overrides.bitYOffsetOverridden = true;
    overrides.ratio = 12;
    overrides.scale = 13;
    overrides.offset = 14;
    overrides.color = std::array<float, 4>{0, 0, 1, 1};
    overrides.bitYOffset = 15;
    auto userSpec = *wave.buffer.channelSpec(0);
    userSpec.ratio = 12;
    userSpec.scale = 13;
    userSpec.offset = 14;
    userSpec.color = overrides.color;
    userSpec.bitDisplay.yOffset = 15;
    wave.buffer.setChannelSpec(0, userSpec);
    // 多次 setup 更新 Lua 默认值，用户显示覆盖不丢失，也不能污染默认基线。
    for (const std::string id : {"enable", "disable", "enable"}) {
        sendControl(id);
        auto expectedDefault = plot::ChannelSpec{
            .label = "default-import-0", .unit = "changed", .ratio = 99, .scale = 88, .offset = 77,
            .color = std::array<float, 4>{1, 0, 0, 1}, .lineWidth = 4.0F,
            .bitDisplay = {.enabled = id == "enable", .firstBit = 6, .bitCount = 3,
                           .yOffset = 9, .hoverReadout = true}};
        auto expectedEffective = expectedDefault;
        expectedEffective.label = data.channels[0].label;
        expectedEffective.ratio = 12;
        expectedEffective.scale = 13;
        expectedEffective.offset = 14;
        expectedEffective.color = std::array<float, 4>{0, 0, 1, 1};
        expectedEffective.bitDisplay.yOffset = 15;
        assertDisplaySpec(wave.defaultChannelSpecs[0], expectedDefault, "覆盖后的默认基线");
        assertDisplaySpec(*wave.buffer.channelSpec(0), expectedEffective, "覆盖后的实际规格");
        assertDisplaySpec(wave.buffer.snapshot(-1e9, 1e9, false).channels[0], expectedEffective,
                          "覆盖后的快照规格");
        assertProtectedData();
    }
    require(plot::resetOneChannelViewSettings(wave, 0), "单通道恢复默认失败");
    auto restored = wave.defaultChannelSpecs[0];
    restored.label = data.channels[0].label;
    assertDisplaySpec(*wave.buffer.channelSpec(0), restored, "恢复最新 Lua 默认");
    assertProtectedData();
    sendControl("omit");
    auto omitted = plot::ChannelSpec{};
    omitted.label = data.channels[0].label;
    assertDisplaySpec(*wave.buffer.channelSpec(0), omitted, "恢复后省略配置");
    // 第一通道合法但第二通道越界时，不得应用任何部分配置。
    const auto beforeInvalidDefaults = wave.defaultChannelSpecs;
    const auto beforeInvalidActual = std::array{*wave.buffer.channelSpec(0), *wave.buffer.channelSpec(1)};
    sendControl("invalid");
    const auto& invalidRows = application.docks().scriptState().rows;
    require(std::any_of(invalidRows.begin(), invalidRows.end(), [](const auto& row) {
        return row.message.find("first_bit + bit_count") != std::string::npos;
    }), "非法 Bit 范围没有明确错误");
    for (std::size_t i = 0; i < 2; ++i) {
        assertDisplaySpec(*wave.buffer.channelSpec(i), beforeInvalidActual[i], "非法配置后的实际规格");
        assertDisplaySpec(wave.defaultChannelSpecs[i], beforeInvalidDefaults[i], "非法配置后的默认规格");
    }
    require(plot::resetAllChannelViewSettings(wave), "全部通道恢复默认失败");
    assertProtectedData();
    sendControl("enable");
    restored = wave.defaultChannelSpecs[0];
    restored.label = data.channels[0].label;
    assertDisplaySpec(*wave.buffer.channelSpec(0), restored, "全部恢复后再次 setup");
    assertProtectedData();
    application.docks().scriptState().rows.clear();

    // 清内存不等于切换来源，明确实时请求之前仍须保留导入身份。
    application.resetWaveHistory();
    require(wave.buffer.dataSource() == plot::WaveDataSource::Imported,
            "手动清空不得隐式切换导入来源");
    // 明确切换到实时来源后，同一 Lua setup 必须恢复普通通道名称和参数行为。
    application.setTransportFactoryForTest([](auto) { return std::make_unique<WriteProbe>(); });
    application.openTransport();
    application.pumpOnce();
    require(!wave.buffer.importedLabelsReadOnly(), "实时来源应解除导入名称保护");
    application.updateControlValue("enable", true);
    waitFor(application, [&] {
        require(application.docks().luaState().lastError.empty(), application.docks().luaState().lastError);
        const auto& rows = application.docks().scriptState().rows;
        return std::any_of(rows.begin(), rows.end(), [](const auto& row) {
            return row.message.find("bit-toggle-enable") != std::string::npos;
        });
    });
    require(wave.buffer.channelCount() == 3 && wave.defaultChannelSpecs.size() == 3,
            "非导入 setup 应恢复其声明的全部通道");
    const std::array<std::string, 3> labels{"renamed", "second", "extra"};
    for (std::size_t i = 0; i < labels.size(); ++i) {
        require(wave.buffer.channelSpec(i)->label == labels[i] && wave.defaultChannelSpecs[i].label == labels[i],
                "非导入 Lua label 应按原顺序写入实际与默认通道名称");
    }
    const auto liveSpec = *wave.buffer.channelSpec(0);
    require(liveSpec.unit == "changed" && liveSpec.ratio == 99 && liveSpec.scale == 88 && liveSpec.offset == 77 &&
                liveSpec.color == std::optional<std::array<float, 4>>{std::array<float, 4>{1, 0, 0, 1}} &&
                liveSpec.lineWidth == 4.0F &&
                liveSpec.bitDisplay.enabled && liveSpec.bitDisplay.firstBit == 6 && liveSpec.bitDisplay.bitCount == 3 &&
                liveSpec.bitDisplay.yOffset == 9 && liveSpec.bitDisplay.hoverReadout,
            "非导入 Lua 通道参数行为不应受名称保护影响");
    const auto liveSnapshot = wave.buffer.snapshot(-1e9, 1e9, false);
    require(liveSnapshot.channels[0].totalSamples == 1 && liveSnapshot.channels[0].samples[0].value == 999,
            "非导入 Lua push 应仍可向原位置通道追加样本");
    application.shutdown();
}

int main(int argc, char**)
{
    try {
        testImportedSourceLabelsAfterReplay();
        testImportedLuaBitToggle();
        testImportedRawRecordsVisibleAndTxBytesPreserved();
        testParsedHistorySwitchTriState();
        testPackageHistoryUsesOfflineSchema();
        testProtocolReloadDefersLargeHistory();
        {
            app::Application historyApplication;
            require(historyApplication.initialize(), "历史预算回归初始化失败");
            app::ApplicationBackpressureTestAccess::verifyHistoryBudgetsAndHandoff(historyApplication);
            historyApplication.shutdown();
        }
        const bool benchmark = argc > 1;
        const std::size_t sampleCount = benchmark ? 1000000 : 25000;
        const std::size_t recordCount = benchmark ? 100000 : 600;
        tests::ScopedTempPath file(tests::makeUniqueTempFile("protoscope-data-task", ".psraw"));
        app::Application application;
        auto& wave = application.docks().waveState();
        plot::WaveCsvData data;
        data.source = "source";
        data.channels.resize(2);
        for (auto& channel : data.channels) {
            channel.label = "duplicate";
            channel.spec.ratio = 2;
            channel.spec.scale = 3;
            channel.spec.offset = 4;
            channel.sampleIndexOffset = 100;
        }
        for (std::size_t i = 0; i < sampleCount; ++i)
            data.channels[0].samples.push_back({static_cast<double>(i / 2), static_cast<double>(i)});
        data.channels[1].samples = {{0, 2}, {1, 3}, {2, 4}};
        std::string error;
        require(application.importWaveCsvData(data, error), error);
        wave.hiddenChannelIndices.push_back(1);
        const auto selected = application.captureWaveData(
            {.kind = plot::CsvExportRangeKind::CursorPair, .cursorATime = 2, .cursorBTime = 1}, error);
        require(selected && selected->channels.size() == 2 && selected->channels[1].samples.size() == 2,
                "hidden channel or closed cursor range lost");
        require(selected->channels[0].samples[0].value == 2, "display transformed value exported");
        require(selected->rangeDescription == "cursors:1,2", "wave range provenance lost");
        const auto coincident = application.captureWaveData(
            {.kind = plot::CsvExportRangeKind::CursorPair, .cursorATime = 1, .cursorBTime = 1}, error);
        require(coincident && coincident->channels[0].samples.size() == 2 &&
                coincident->channels[1].samples.size() == 1, "coincident cursor endpoint samples lost");
        wave.view.sampleFrequencyHz = 10;
        const auto frequency = application.captureWaveData(
            {.kind = plot::CsvExportRangeKind::CurrentView, .currentViewMinTime = 10.1, .currentViewMaxTime = 10.2}, error);
        require(frequency && frequency->channels[0].samples.size() == 2 &&
            frequency->channels[0].sampleIndexOffset == 101, "frequency offset range wrong");
        wave.view.sampleFrequencyHz = 0;
        plot::RawCaptureFileData capture;
        capture.waveform = data;
        const auto started = Clock::now();
        require(plot::writeRawCaptureFile(file.path(), capture, error), error);
        const double writeMs = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        wave.buffer.clear();
        wave.buffer.appendImported(0, {{9, 9}});
        application.docks().receiveState().rows.push_back({.direction = "TX", .bytes = {7}});
        wave.rawCapture.events.push_back({.type = plot::RawCaptureEventType::TxBytes, .bytes = {7}});
        std::size_t openCalls = 0;
        application.setTransportFactoryForTest([&](auto) -> std::unique_ptr<transport::ITransport> {
            ++openCalls; return {};
        });
        const auto importStarted = Clock::now();
        require(application.startDataImport(file.path(), error), error);
        waitFor(application, [&] { const auto s = application.dataTransferStatus();
            return s.awaitingConfirmation || s.complete; });
        require(application.dataTransferStatus().awaitingConfirmation, application.dataTransferStatus().error);
        require(wave.buffer.snapshot(-1e9, 1e9, false).channels[0].totalSamples == 1, "cleared before confirmation");
        application.openTransport();
        require(openCalls == 0 && !application.sendManualPayload("unsafe", false), "physical I/O during import");
        application.confirmDataImport();
        double maxPumpMs = 0;
        waitFor(application, [&] {
            const auto before = application.dataTransferStatus().submitted;
            const auto begin = Clock::now();
            application.pumpOnce();
            maxPumpMs = (std::max)(maxPumpMs, std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
            const auto current = application.dataTransferStatus();
            require(current.submitted - before <= 8192, "unbounded sample submission");
            return current.complete && !current.active;
        }, false);
        const double importMs = std::chrono::duration<double, std::milli>(Clock::now() - importStarted).count();
        const auto restored = application.captureWaveData({}, error);
        require(restored && restored->channels[0].samples.size() == sampleCount, "sample loss in import");
        require(restored->source == "source", "source loss in import");
        require(application.docks().receiveState().rows.size() == 1 &&
            wave.rawCapture.events.size() == 1, "wave import replaced records");
        auto spec = *wave.buffer.channelSpec(0);
        spec.label = "changed";
        spec.offset = 12;
        wave.buffer.setChannelSpec(0, spec);
        require(wave.buffer.channelSpec(0)->label == "duplicate" && wave.buffer.channelSpec(0)->offset == 12,
                "label protection or display editing broken");
        wave.buffer.setMaxTotalSamples(1);
        require(wave.buffer.snapshot(-1e9, 1e9, false).channels[0].totalSamples == sampleCount, "import history trimmed");

        capture = {};
        capture.protocolDir = "missing-protocol";
        capture.rxOnly = false;
        for (std::size_t i = 0; i < recordCount; ++i)
            capture.events.push_back({.type = i % 2 ? plot::RawCaptureEventType::TxBytes : plot::RawCaptureEventType::RxBytes,
                .timestampMs = i / 2, .bytes = std::vector<std::uint8_t>(benchmark ? 256 : 3, static_cast<std::uint8_t>(i)),
                .endpoint = "COM1", .sequence = i});
        require(plot::writeRawCaptureFile(file.path(), capture, error), error);
        const auto recordsStarted = Clock::now();
        require(application.startDataImport(file.path(), error), error);
        waitFor(application, [&] { return application.dataTransferStatus().awaitingConfirmation; });
        application.confirmDataImport();
        waitFor(application, [&] { auto s = application.dataTransferStatus(); return s.complete && !s.active; });
        application.pumpOnce();
        const double recordsMs = std::chrono::duration<double, std::milli>(Clock::now() - recordsStarted).count();
        require(wave.rawCapture.events.size() == recordCount, "records dropped");
        require(wave.buffer.snapshot(-1e9, 1e9, false).channels[0].totalSamples == sampleCount, "record import replaced wave");
        require(wave.rawCapture.events[1].type == plot::RawCaptureEventType::TxBytes && openCalls == 0, "TX replay sent");
        application.docks().receiveState().filter.status = dock::LogStatusFilter::Tx;
        require(application.startDataExport(file.path(), 1, 1, {}, 1, 0, 0, plot::WaveCsvShape::Wide, error), error);
        waitFor(application, [&] { return !application.dataTransferStatus().active; }, false);
        const auto filtered = plot::readRawCaptureFile(file.path(), error);
        require(filtered && filtered->events.size() == recordCount / 2 && filtered->payload.empty() && filtered->filtered,
                "raw direction filter failed");
        require(filtered->rangeDescription == "filter", "record range provenance lost");
        application.docks().receiveState().frameRows.push_back(
            {.timestampMs = 2, .direction = "RX", .bytes = {0x11}, .message = "frame,\"parsed\"\nvalue=17"});
        require(!application.startDataExport(file.path(), 2, 0, {}, 2, 3, 1, plot::WaveCsvShape::Long, error),
            "invalid analysis range accepted");
        error.clear();
        require(application.startDataExport(file.path(), 2, 0, {}, 0, 0, 0, plot::WaveCsvShape::Long, error), error);
        waitFor(application, [&] { return !application.dataTransferStatus().active; }, false);
        require(plot::detectCsvKind(file.path(), error) == plot::CsvKind::Unknown, "analysis CSV accepted as raw input");
        std::ifstream analysisFile(file.path(), std::ios::binary);
        const std::string analysisText((std::istreambuf_iterator<char>(analysisFile)), {});
        require(analysisText.find("value=17") != std::string::npos &&
            analysisText.find("\"\"parsed\"\"") != std::string::npos, "analysis output or CSV quoting lost");
        analysisFile.close();
        error.clear();

        require(plot::writeRawCaptureFile(file.path(), {.waveform = data}, error), error);
        require(application.startDataImport(file.path(), error), error);
        waitFor(application, [&] { return application.dataTransferStatus().awaitingConfirmation; });
        application.confirmDataImport();
        waitFor(application, [&] { return application.dataTransferStatus().submitted >= 8192; });
        const auto cancelStarted = Clock::now();
        application.cancelDataTransfer();
        waitFor(application, [&] { auto s = application.dataTransferStatus(); return s.complete && !s.active; });
        application.pumpOnce();
        const double cancelMs = std::chrono::duration<double, std::milli>(Clock::now() - cancelStarted).count();
        const auto partial = application.captureWaveData({}, error);
        require(partial && partial->incomplete && !partial->channels[0].samples.empty(), "partial import not preserved");
        require(plot::writeWaveCsvFile(file.path(), *partial, plot::WaveCsvShape::Long, {}, error), error);
        const auto reread = plot::readWaveCsvFile(file.path(), error);
        require(reread && reread->incomplete, "partial re-export lost integrity flag");
        require(application.startDataExport(file.path(), 3, 3, {}, 0, 0, 0, plot::WaveCsvShape::Long, error), error);
        waitFor(application, [&] { return !application.dataTransferStatus().active; }, false);
        const auto beforeSession = application.captureWaveData({}, error);
        require(application.startDataImport(file.path(), error), error);
        waitFor(application, [&] { return application.dataTransferStatus().awaitingConfirmation; });
        application.confirmDataImport();
        waitFor(application, [&] { auto s = application.dataTransferStatus(); return s.complete && !s.active; });
        const auto afterSession = application.captureWaveData({}, error);
        require(afterSession && afterSession->channels[0].samples.size() == beforeSession->channels[0].samples.size() &&
            wave.rawCapture.events.size() == recordCount, "session snapshot or records lost");
        config::AppConfig preferences;
        preferences.gui.lastDataExport = {.valid = true, .content = 1, .format = 1,
            .waveRange = 2, .recordRange = 1, .csvShape = 1, .directory = "captures"};
        config::ConfigStore store;
        std::string yaml;
        require(store.saveText(preferences, yaml, error), error);
        const auto loaded = store.loadText(yaml);
        require(loaded.error.empty() && loaded.config.gui.lastDataExport.valid &&
            loaded.config.gui.lastDataExport.directory == "captures" &&
            loaded.config.gui.lastDataExport.waveRange == 2, "export preferences not persistent");
        app::Application writer;
        writer.setTransportFactoryForTest([](auto) { return std::make_unique<WriteProbe>(); });
        writer.openTransport();
        writer.pumpOnce();
        require(writer.startRawCaptureRecording(file.path(), error), error);
        require(!writer.sendManualPayload("abc", false), "partial write reported success");
        writer.pumpOnce();
        writer.pumpOnce();
        const auto& written = writer.docks().waveState().rawCapture.events;
        require(written.size() == 1 && written[0].type == plot::RawCaptureEventType::TxBytes &&
            written[0].bytes == std::vector<std::uint8_t>({'a', 'b'}) && written[0].writeStatus == "partial",
            "actual TX lost or duplicated");
        require(writer.stopRawCaptureRecording(error), error);
        const auto recording = plot::readRawCaptureFile(file.path(), error);
        require(recording && recording->events.size() == 1 && recording->events[0].bytes.size() == 2 &&
            recording->events[0].endpoint == "probe", "TX recording lost or duplicated");
        writer.closeTransport();
        app::Application boundaryImporter;
        capture = {};
        capture.events = {
            {.type = plot::RawCaptureEventType::RxBytes,
             .bytes = std::vector<std::uint8_t>(65536, 0x11), .sequence = 1},
            {.type = plot::RawCaptureEventType::RxBytes,
             .bytes = std::vector<std::uint8_t>(131072, 0x22), .sequence = 2},
            {.type = plot::RawCaptureEventType::TxBytes,
             .bytes = std::vector<std::uint8_t>(65536, 0x33), .sequence = 3},
        };
        require(plot::writeRawCaptureFile(file.path(), capture, error), error);
        require(boundaryImporter.startDataImport(file.path(), error), error);
        waitFor(boundaryImporter, [&] { return boundaryImporter.dataTransferStatus().awaitingConfirmation; });
        boundaryImporter.confirmDataImport();
        waitFor(boundaryImporter, [&] {
            const auto status = boundaryImporter.dataTransferStatus();
            return status.complete && !status.active;
        });
        boundaryImporter.pumpOnce();
        const auto& boundaryEvents = boundaryImporter.docks().waveState().rawCapture.events;
        require(boundaryEvents.size() == 3 &&
                boundaryEvents[0].sequence == 1 && boundaryEvents[0].bytes.size() == 65536 &&
                boundaryEvents[1].sequence == 2 && boundaryEvents[1].bytes.size() == 131072 &&
                boundaryEvents[2].sequence == 3 && boundaryEvents[2].bytes.size() == 65536,
                "64 KiB boundary events lost, merged, or split");
        boundaryImporter.shutdown();

        app::Application parser;
        std::size_t parserOpenCalls = 0;
        parser.setTransportFactoryForTest([&](auto) {
            ++parserOpenCalls;
            return std::make_unique<WriteProbe>();
        });
        const auto protocol = std::filesystem::path(__FILE__).parent_path() / "fixtures/protocols/data_transfer";
        require(parser.reloadProtocolDirectory(protocol.generic_string(), true), "offline parser fixture did not load");
        capture = {};
        capture.events = {
            {.type = plot::RawCaptureEventType::RxBytes, .bytes = {11, 12, 13}},
            {.type = plot::RawCaptureEventType::TxBytes, .bytes = {99}},
        };
        require(plot::writeRawCaptureFile(file.path(), capture, error), error);
        require(parser.startDataImport(file.path(), error), error);
        waitFor(parser, [&] { return parser.dataTransferStatus().awaitingConfirmation; });
        parser.confirmDataImport(true);
        waitFor(parser, [&] { auto s = parser.dataTransferStatus(); return s.complete && !s.active; });
        for (int i = 0; i < 5; ++i) parser.pumpOnce();
        const auto parsed = parser.captureWaveData({}, error);
        require(parsed && parsed->channels[0].samples.size() == 3 &&
            parsed->channels[0].samples.back().value == 13,
            "explicit RX parsing failed or parsed TX: " + error + " lua=" + parser.docks().luaState().lastError);
        require(parserOpenCalls == 0 && parser.docks().waveState().rawCapture.events.size() == 2,
            "offline script send/request reached transport or changed records");
        capture.events.push_back({.type = plot::RawCaptureEventType::PlotSetup,
            .plotSetup = {.channels = {plot::ChannelSpec{.label = "reset"}}, .resetHistory = true}});
        require(plot::writeRawCaptureFile(file.path(), capture, error), error);
        require(parser.startDataImport(file.path(), error), error);
        waitFor(parser, [&] { return parser.dataTransferStatus().awaitingConfirmation; });
        parser.confirmDataImport(true);
        waitFor(parser, [&] { auto s = parser.dataTransferStatus(); return s.complete && !s.active; });
        const auto resetWave = parser.docks().waveState().buffer.snapshot(-1e9, 1e9, false);
        require(resetWave.channels.size() == 1 && resetWave.channels[0].totalSamples == 0,
            "plot setup overtook preceding RX callback");
        parser.shutdown();
        app::Application sender;
        WriteProbe* probe = nullptr;
        sender.setTransportFactoryForTest([&](auto) {
            auto value = std::make_unique<WriteProbe>();
            probe = value.get();
            return value;
        });
        require(sender.reloadProtocolDirectory(protocol.generic_string(), true), "sender fixture did not load");
        sender.openTransport();
        sender.pumpOnce();
        require(sender.startRawCaptureRecording(file.path(), error), error);
        sender.updateControlValue("send", true);
        waitFor(sender, [&] { return sender.docks().waveState().rawCapture.events.size() >= 1; });
        sender.updateControlValue("retry", true);
        waitFor(sender, [&] { return sender.docks().waveState().rawCapture.events.size() >= 3; });
        // 等待重试完成回调，避免下一条普通发送仍被半双工请求阻塞。
        for (int i = 0; i < 30; ++i) {
            sender.pumpOnce();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        probe->writeDelayMs = 10;
        sender.updateControlValue("timeout", true);
        waitFor(sender, [&] { return sender.docks().waveState().rawCapture.events.size() >= 4; });
        probe->writeLimit = 0;
        require(!sender.sendManualPayload("fail", false), "zero-byte failure succeeded");
        sender.pumpOnce();
        const auto& tx = sender.docks().waveState().rawCapture.events;
        require(tx.size() == 5 && tx[0].bytes == std::vector<std::uint8_t>({0x21, 0x22}) &&
            tx[1].bytes == std::vector<std::uint8_t>({0x31}) && tx[2].bytes == tx[1].bytes &&
            tx[3].writeStatus == "written_timeout" && tx[4].writeStatus == "not_sent" && tx[4].bytes.empty(),
            "script/retry/timeout/failure TX count or physical bytes wrong");
        require(sender.stopRawCaptureRecording(error), error);
        const auto allTx = plot::readRawCaptureFile(file.path(), error);
        require(allTx && allTx->events.size() == 5, "TX recording duplicated or missed attempts");
        sender.shutdown();
        const auto totalMs = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        std::cout << "samples=" << sampleCount << " write_ms=" << writeMs << " import_ms=" << importMs
                  << " max_pump_ms=" << maxPumpMs << " records=" << recordCount << " records_import_ms=" << recordsMs
                  << " cancel_ms=" << cancelMs << " total_ms=" << totalMs;
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS memory{};
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
            std::cout << " peak_working_set_bytes=" << memory.PeakWorkingSetSize;
#endif
        std::cout << '\n';
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
