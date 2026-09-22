#include "../src/ui/wave/wave_render_service.hpp"
#include "protoscope/config/config.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace protoscope;
using plot::WaveDownsampleMode;

namespace {
void require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}

void testConfig()
{
    config::ConfigStore configs;
    for (const auto text : {"stable_edges", "legacy_uniform", "unknown", ""}) {
        const auto loaded = configs.loadText(std::string("gui:\n  wave:\n    downsample_mode: '") + text + "'\n");
        const auto expected = std::string_view(text) == "legacy_uniform"
            ? WaveDownsampleMode::LegacyUniform : WaveDownsampleMode::StableEdges;
        require(loaded.error.empty() && loaded.config.gui.wave.downsampleMode == expected, "config parse");
        dock::DockStore docks;
        configs.applyToDock(loaded.config, docks);
        require(docks.waveState().view.downsampleMode == expected, "config apply");
        const auto captured = configs.captureFromDock(docks);
        require(captured.gui.wave.downsampleMode == expected, "config capture");
        std::string yaml, error;
        require(configs.saveText(captured, yaml, error), "config save");
        require(configs.loadText(yaml).config.gui.wave.downsampleMode == expected, "config roundtrip");
        const auto path = std::filesystem::temp_directory_path() /
            ("protoscope-downsample-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".yaml");
        require(configs.save(path, captured, error), "save config to disk");
        const auto fromDisk = configs.load(path);
        require(fromDisk.loadedFromDisk && fromDisk.config.gui.wave.downsampleMode == expected, "startup disk load");
        configs.applyToDock(configs.loadText("gui:\n  wave:\n    peak_detect_downsample: false\n").config, docks);
        require(docks.waveState().view.downsampleMode == WaveDownsampleMode::StableEdges, "missing mode on reload");
    }
    require(configs.loadText("{}").config.gui.wave.downsampleMode == WaveDownsampleMode::StableEdges,
            "missing wave config default");
}

void testDisplaySwitch()
{
    plot::WaveDockState wave;
    auto& view = wave.view;
    view.initialized = true;
    view.defaultViewportPending = false;
    view.autoFollowLatest = false;
    view.glowEnabled = false;
    view.viewMinTime = 100.25;
    view.viewMaxTime = 8100.75;
    view.visibleDuration = view.viewMaxTime - view.viewMinTime;
    view.maxRenderPointsPerChannel = 80;
    view.cursors[0].time = 1234;
    std::vector<plot::WaveSample> samples;
    for (int i = 0; i < 10000; ++i) samples.push_back({double(i), double(i >= 1234 && i < 4567)});
    wave.buffer.append(0, {{}, samples});
    wave.buffer.append(1, {{}, samples});
    wave.buffer.setChannelSpec(1, {.bitDisplay = {.enabled = true, .bitCount = 1}});
    std::vector<std::size_t> stable, digital;
    auto previousMode = view.downsampleMode;
    std::uint64_t generation = 0;
    for (auto mode : {WaveDownsampleMode::StableEdges, WaveDownsampleMode::LegacyUniform,
                      WaveDownsampleMode::StableEdges, WaveDownsampleMode::LegacyUniform}) {
        view.downsampleMode = mode;
        const auto frame = ui::prepareWaveFrame(wave, 800);
        if (mode != previousMode) ++generation;
        require(view.phosphorResetGeneration == generation, "mode must invalidate frozen phosphor");
        require(wave.renderEnvelopeCache.empty() && wave.overviewRenderCache.empty(), "mode clears render caches");
        require(wave.cachedDisplayKey.downsampleMode == mode && wave.cachedOverviewKey.downsampleMode == mode,
                "display keys include mode");
        const auto& display = frame.displayData->channels[0];
        const plot::WaveQueryView query(*display.source, display.axis, display.frequency, display.formula);
        const auto& source = frame.snapshot.channels[0];
        const auto expected = mode == WaveDownsampleMode::StableEdges
            ? query.traceIndices(view.viewMinTime, view.viewMaxTime, frame.renderBudget.pointsPerChannel)
            : query.traceIndices(query.time(source.visibleBegin), query.time(source.visibleEnd - 1),
                                 frame.renderBudget.pointsPerChannel, nullptr, false, mode);
        require(display.sourceIndices == expected, "display uses correct range and strategy");
        const auto& overview = frame.overviewDisplayData->channels[0];
        require(overview.sourceIndices == query.traceIndices(query.time(0), query.time(query.size() - 1),
                    wave.cachedOverviewKey.pointLimit, nullptr, false, mode), "overview uses same strategy");
        if (stable.empty()) {
            stable = display.sourceIndices;
            digital = frame.displayData->channels[1].sourceIndices;
        }
        require(mode != WaveDownsampleMode::StableEdges || display.sourceIndices == stable,
                "switch back restores stable indices");
        require(mode != WaveDownsampleMode::LegacyUniform || display.sourceIndices != stable,
                "fixture distinguishes algorithms");
        require(frame.displayData->channels[1].sourceIndices == digital, "digital path independent of mode");
        require(view.cursors[0].time == 1234, "mode preserves cursor");
        const auto exact = plot::findNearestDisplayByTime(*frame.displayData, 0, 1234, 0);
        require(exact && exact->value == 1, "readout uses original samples");
        const auto fftGeneration = wave.fftRequestGeneration;
        const auto measurementGeneration = wave.measurementRequestGeneration;
        wave.renderEnvelopeCache.resize(1);
        wave.renderEnvelopeCache[0].valid = true;
        wave.overviewRenderCache.resize(1);
        wave.overviewRenderCache[0].valid = true;
        ui::prepareWaveFrame(wave, 800);
        require(wave.renderEnvelopeCache[0].valid && wave.overviewRenderCache[0].valid, "same mode reuses caches");
        require(wave.fftRequestGeneration == fftGeneration && wave.measurementRequestGeneration == measurementGeneration,
                "display switch does not resubmit analysis");
        previousMode = mode;
    }
}

void testLegacyDrawingGolden()
{
    // e6320e1 查询结果再进入当时的绘制函数，验证第二次取点和包络桶的固定输出。
    std::vector<plot::WaveSample> trace;
    for (int i : {3, 8, 11, 12, 16, 19, 20, 22, 27, 28})
        trace.push_back({double(i), double(i * 7 % 19 - 9)});
    const auto peak = ui::buildPeakDetectDownsample(trace, 3.5, 27.5, 8);
    std::vector<double> times;
    for (const auto& sample : peak) times.push_back(sample.time);
    require(times == std::vector<double>{3, 8, 11, 16, 19, 27, 28}, "legacy drawing peak golden");
    const auto envelope = ui::buildDisplayEnvelope(trace, 3.5, 27.5, 4);
    const std::vector<plot::EnvelopePoint> expected{
        {5.5, -7, 9, 2}, {13, -8, 8, 3}, {19.5, -9, -2, 2}, {77.0 / 3, -7, 9, 3}};
    require(envelope.size() == expected.size(), "legacy drawing envelope size");
    for (std::size_t i = 0; i < expected.size(); ++i)
        require(envelope[i].time == expected[i].time && envelope[i].minValue == expected[i].minValue &&
                envelope[i].maxValue == expected[i].maxValue && envelope[i].sampleCount == expected[i].sampleCount,
                "legacy drawing envelope golden");
    plot::WaveDockState::RenderEnvelopeCacheKey a, b;
    b.downsampleMode = WaveDownsampleMode::LegacyUniform;
    require(!(a == b), "envelope cache key separates modes");
    plot::WaveDockState::OverviewRenderKey c, d;
    d.downsampleMode = WaveDownsampleMode::LegacyUniform;
    require(!(c == d), "overview render key separates modes");
}
}

int main()
{
    try {
        testConfig();
        testDisplaySwitch();
        testLegacyDrawingGolden();
        std::cout << "wave downsample mode: all checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
