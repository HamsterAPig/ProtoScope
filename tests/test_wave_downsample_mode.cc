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
    require(config::AppConfig{}.gui.wave.downsampleMode == WaveDownsampleMode::LegacyUniform,
            "AppConfig 默认应使用 LegacyUniform");
    require(plot::WaveViewState{}.downsampleMode == WaveDownsampleMode::LegacyUniform,
            "WaveViewState 默认应使用 LegacyUniform");
    config::ConfigStore configs;
    for (const auto text : {"stable_edges", "legacy_uniform", "unknown", ""}) {
        const auto loaded = configs.loadText(std::string("gui:\n  wave:\n    downsample_mode: '") + text + "'\n");
        const auto expected = std::string_view(text) == "stable_edges"
            ? WaveDownsampleMode::StableEdges : WaveDownsampleMode::LegacyUniform;
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
        require(docks.waveState().view.downsampleMode == WaveDownsampleMode::LegacyUniform, "missing mode on reload");
    }
    require(configs.loadText("{}").config.gui.wave.downsampleMode == WaveDownsampleMode::LegacyUniform,
            "missing wave config default");
}

void testDefaultQueryMode()
{
    // 窄预算与远离桶边界的阶跃确保真正降采样，且两种算法不能碰巧给出相同结果。
    plot::OscilloscopeBuffer buffer;
    std::vector<plot::WaveSample> samples;
    for (int i = 0; i < 10000; ++i) samples.push_back({double(i), double(i >= 1234 && i < 4567)});
    buffer.append(0, {{}, samples});
    constexpr std::size_t budget = 80;
    constexpr double minTime = 100.25, maxTime = 8100.75;
    const auto snapshot = buffer.snapshot(minTime, maxTime, false);
    const plot::WaveQueryView query(snapshot.channels[0], plot::WaveTimeAxisSource::ScriptTime,
                                    0, snapshot.config.displayFormula);
    const auto implicit = query.traceIndices(minTime, maxTime, budget);
    const auto legacy = query.traceIndices(minTime, maxTime, budget, nullptr, true,
                                           WaveDownsampleMode::LegacyUniform);
    const auto stable = query.traceIndices(minTime, maxTime, budget, nullptr, true,
                                           WaveDownsampleMode::StableEdges);
    require(!implicit.empty() && implicit.size() <= budget && implicit.size() < samples.size(),
            "默认查询必须执行非平凡降采样并遵守预算");
    require(implicit == legacy, "traceIndices 省略模式应等同显式 LegacyUniform");
    require(implicit != stable, "查询样本必须区分默认模式与 StableEdges");

    // 同时覆盖省略全部可选参数及只显式传入视口的调用形式。
    for (const auto range : {std::optional<std::pair<double, double>>{},
                             std::optional{std::pair{minTime, maxTime}}}) {
        plot::WaveDisplayData defaultData, legacyData, stableData;
        if (range)
            plot::buildQueryDisplayDataInto(snapshot, 0, budget, defaultData, range);
        else
            plot::buildQueryDisplayDataInto(snapshot, 0, budget, defaultData);
        plot::buildQueryDisplayDataInto(snapshot, 0, budget, legacyData, range, WaveDownsampleMode::LegacyUniform);
        plot::buildQueryDisplayDataInto(snapshot, 0, budget, stableData, range, WaveDownsampleMode::StableEdges);
        const auto& actual = defaultData.channels[0];
        const auto& expected = legacyData.channels[0];
        require(!actual.samples.empty() && actual.samples.size() <= budget && actual.samples.size() < samples.size(),
                "默认显示构建必须执行非平凡降采样并遵守预算");
        require(actual.sourceIndices == expected.sourceIndices && actual.actualValues == expected.actualValues &&
                    actual.samples.size() == expected.samples.size(),
                "buildQueryDisplayDataInto 省略模式应等同显式 LegacyUniform");
        require(actual.sourceIndices != stableData.channels[0].sourceIndices,
                "显示样本必须区分默认模式与 StableEdges");
        for (std::size_t i = 0; i < actual.samples.size(); ++i)
            require(actual.samples[i].time == expected.samples[i].time &&
                        actual.samples[i].value == expected.samples[i].value,
                    "默认显示轨迹时间和值应与 LegacyUniform 一致");
    }
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
            ? query.traceIndices(view.viewMinTime, view.viewMaxTime, frame.renderBudget.pointsPerChannel,
                                 nullptr, true, WaveDownsampleMode::StableEdges)
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
    a.downsampleMode = WaveDownsampleMode::StableEdges;
    b.downsampleMode = WaveDownsampleMode::LegacyUniform;
    require(!(a == b), "envelope cache key separates modes");
    plot::WaveDockState::OverviewRenderKey c, d;
    c.downsampleMode = WaveDownsampleMode::StableEdges;
    d.downsampleMode = WaveDownsampleMode::LegacyUniform;
    require(!(c == d), "overview render key separates modes");
}
}

int main()
{
    try {
        testConfig();
        testDefaultQueryMode();
        testDisplaySwitch();
        testLegacyDrawingGolden();
        std::cout << "wave downsample mode: all checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
