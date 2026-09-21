#include "protoscope/app/application.hpp"
#include "protoscope/plot/csv_data_file.hpp"
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
    for (const std::string id : {"enable", "disable", "enable", "omit"}) {
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
        require(wave.buffer.channelSpec(0)->bitDisplay.enabled == (id == "enable"), "Lua Bit toggle not applied");
        require(wave.buffer.channelSpec(1)->bitDisplay.enabled, "unmentioned imported channel changed");
        const auto snapshot = wave.buffer.snapshot(-1e9, 1e9, false);
        require(snapshot.channels.size() == 2, "Lua changed imported channel count");
        for (std::size_t i = 0; i < 2; ++i) {
            auto expected = data.channels[i].spec;
            expected.label = data.channels[i].label;
            expected.unit = data.channels[i].unit;
            expected.bitDisplay.enabled = i == 1 || id == "enable";
            const auto actual = *wave.buffer.channelSpec(i);
            require(actual.label == expected.label && actual.unit == expected.unit &&
                    actual.ratio == expected.ratio && actual.scale == expected.scale && actual.offset == expected.offset &&
                    actual.color == expected.color && actual.lineWidth == expected.lineWidth &&
                    actual.bitDisplay == expected.bitDisplay, "Lua modified protected channel metadata");
            require(wave.defaultChannelSpecs[i].bitDisplay == expected.bitDisplay &&
                    wave.defaultChannelSpecs[i].label == expected.label, "default Bit spec not synchronized");
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
                wave.buffer.viewConfig().historyLimit == originalConfig.historyLimit &&
                wave.buffer.viewConfig().verticalMin == originalConfig.verticalMin,
                "Lua changed imported view config");
        require(wave.view.initialized && !wave.view.defaultViewportPending &&
                wave.view.viewMinTime == 1.5 && wave.view.viewMaxTime == 2.5, "Lua reset viewport");
        require(wave.view.sampleFrequencyHz == 10 &&
                wave.view.timeAxisSource == plot::WaveTimeAxisSource::SampleFrequency,
                "Lua changed imported time axis");
        require(wave.rawCapture.events.size() == originalEvents, "unapplied Lua setup recorded in raw capture");
    }
    application.shutdown();
}

int main(int argc, char**)
{
    try {
        testImportedLuaBitToggle();
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
