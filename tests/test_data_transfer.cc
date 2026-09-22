#include "protoscope/plot/csv_data_file.hpp"
#include "protoscope/plot/data_file_output.hpp"
#include "test_helpers.hpp"

#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace protoscope::plot;

static void check(bool ok, const std::string& message)
{
    if (!ok) throw std::runtime_error(message);
}

int main()
{
    try {
        WaveCsvData wave;
        wave.source = "test\nsource";
        wave.sampleFrequencyHz = 1234.5678901234567;
        wave.timeAxis = "frequency";
        wave.view.displayFormula = WaveDisplayFormula::ScaleThenOffset;
        wave.channels.resize(3);
        for (auto& channel : wave.channels) {
            channel.label = "time";
            channel.unit = "V\nraw";
            channel.spec.ratio = 3.0;
            channel.spec.scale = 4.0;
            channel.spec.offset = 5.0;
            channel.spec.bitDisplay.enabled = true;
            channel.sampleIndexOffset = 17;
        }
        wave.channels[0].samples = {{1, 2}, {1, 3}, {std::nextafter(1.0, 2.0), 4}};
        wave.channels[1].samples = {{1, 8}, {2, 9}};
        for (auto shape : {WaveCsvShape::Wide, WaveCsvShape::Long}) {
            std::string error;
            std::ostringstream text;
            check(encodeWaveCsv(text, wave, shape, {}, error), error);
            const auto restored = decodeWaveCsv(text.str(), error);
            check(restored.has_value(), error);
            check(restored->channels.size() == 3, "empty channel lost");
            check(restored->source == wave.source, "source lost");
            check(restored->timeAxis == wave.timeAxis, "axis lost");
            check(restored->view.displayFormula == wave.view.displayFormula, "formula lost");
            for (std::size_t c = 0; c < wave.channels.size(); ++c) {
                const auto& actual = restored->channels[c];
                const auto& expected = wave.channels[c];
                check(actual.samples.size() == expected.samples.size(), "duplicate sample lost");
                check(actual.label == expected.label && actual.unit == expected.unit, "identity lost");
                check(actual.spec.ratio == 3 && actual.spec.scale == 4 && actual.spec.offset == 5, "raw transform lost");
                check(actual.sampleIndexOffset == 17 && actual.spec.bitDisplay.enabled, "channel attributes lost");
                for (std::size_t i = 0; i < expected.samples.size(); ++i)
                    check(actual.samples[i].time == expected.samples[i].time &&
                          actual.samples[i].value == expected.samples[i].value, "sample changed");
            }
        }
        RawCaptureFileData capture;
        capture.waveform = wave;
        capture.source = "fixture";
        capture.incomplete = true;
        capture.filtered = true;
        capture.rxOnly = false;
        capture.events = {
            {.type = RawCaptureEventType::RxBytes, .timestampMs = 10, .bytes = {0, 255},
             .endpoint = "COM1", .sequence = 9},
            {.type = RawCaptureEventType::TxBytes, .timestampMs = 10, .bytes = {4, 0},
             .endpoint = "COM1", .sequence = 10, .writeStatus = "partial"},
        };
        std::string error;
        std::vector<std::uint8_t> bytes;
        check(encodeRawCaptureFile(capture, bytes, error), error);
        const auto restored = decodeRawCaptureFile(
            {reinterpret_cast<const char*>(bytes.data()), bytes.size()}, error);
        check(restored.has_value(), error);
        check(restored->waveform && restored->waveform->channels[0].samples.size() == 3, "snapshot lost");
        check(restored->events.size() == 2 && restored->payload == capture.events[0].bytes, "RX payload polluted");
        check(restored->events[1].bytes == capture.events[1].bytes &&
              restored->events[1].type == RawCaptureEventType::TxBytes, "TX lost");
        check(restored->events[1].sequence == 10 && restored->events[1].endpoint == "COM1" &&
              restored->events[1].writeStatus == "partial", "event metadata lost");
        check(restored->incomplete && restored->filtered && !restored->rxOnly, "provenance lost");
        protoscope::tests::ScopedTempPath file(protoscope::tests::makeUniqueTempFile("protoscope-data-csv", ".csv"));
        capture.source = "source\nwith newline";
        capture.events[1].endpoint = std::string(70000, 'x') + "\n\"endpoint";
        check(writeRawCaptureCsvFile(file.path(), capture, {}, error), error);
        const auto csvRaw = readRawCaptureCsvFile(file.path(), error);
        check(csvRaw && csvRaw->events.size() == 2 && csvRaw->events[1].endpoint == capture.events[1].endpoint,
              "cross-block quoted field lost");
        check(csvRaw->events[1].bytes == capture.events[1].bytes &&
              csvRaw->events[1].sequence == 10 && csvRaw->source == capture.source, "raw CSV roundtrip changed");
        std::stop_source stop;
        stop.request_stop();
        dataFileStopToken() = stop.get_token();
        check(!writeWaveCsvFile(file.path(), wave, WaveCsvShape::Long, {}, error), "canceled export succeeded");
        dataFileStopToken() = {};
        const auto preserved = readRawCaptureCsvFile(file.path(), error);
        check(preserved && preserved->events.size() == 2, "canceled export destroyed target");
        check(!validateCsvExportRange({.kind = CsvExportRangeKind::CurrentView,
            .currentViewMinTime = NAN}, error), "invalid range accepted");
        WaveCsvData largeWave;
        largeWave.channels.resize(1);
        largeWave.channels[0].label = "stream";
        for (std::size_t i = 0; i < 20000; ++i)
            largeWave.channels[0].samples.push_back({static_cast<double>(i), static_cast<double>(i)});
        std::ostringstream largeText;
        check(encodeWaveCsv(largeText, largeWave, WaveCsvShape::Long, {}, error), error);
        std::istringstream stream(largeText.str());
        std::size_t streamedSamples = 0;
        bool earlyMetadata = false;
        DataFileReadCallbacks callbacks;
        callbacks.metadata = [&](const RawCaptureFileData&, bool records) {
            earlyMetadata = !records && stream.tellg() < static_cast<std::streamoff>(largeText.str().size() / 2);
            return true;
        };
        callbacks.samples = [&](std::size_t c, std::size_t, std::vector<WaveSample> samples) {
            check(c == 0 && samples.size() <= 8192, "unbounded waveform batch");
            streamedSamples += samples.size();
            return true;
        };
        check(readWaveCsvStream(stream, error, &callbacks).has_value() &&
              earlyMetadata && streamedSamples == 20000, "waveform did not stream after header");
        // 损坏发生在后续块时，之前已经交付的批次仍然可保留。
        std::istringstream damaged(largeText.str() + "1,stream,,invalid,1\n");
        streamedSamples = 0;
        check(!readWaveCsvStream(damaged, error, &callbacks) && streamedSamples >= 8192,
              "damaged tail discarded already parsed waveform");
        capture = {};
        capture.events.push_back({.type = RawCaptureEventType::TxBytes,
            .bytes = std::vector<std::uint8_t>(150000, 0xA5), .sequence = 42});
        check(writeRawCaptureFile(file.path(), capture, error), error);
        std::size_t streamedBytes = 0;
        std::size_t events = 0;
        callbacks.metadata = [](const RawCaptureFileData&, bool records) { return records; };
        callbacks.event = [&](RawCaptureEvent event, bool continuation) {
            check(event.bytes.size() <= 65536 && event.sequence == 42, "unbounded or changed event");
            streamedBytes += event.bytes.size();
            if (!continuation) ++events;
            return true;
        };
        const auto streamedRaw = readRawCaptureFile(file.path(), error, &callbacks);
        check(streamedRaw && streamedRaw->events.empty() && events == 1 && streamedBytes == 150000,
              "raw stream accumulated or duplicated events");
        std::cout << "data transfer regression passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
