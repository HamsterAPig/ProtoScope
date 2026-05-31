// 本文件由 wave_dock_renderer.cpp 按原顺序包含，承接对应 Wave 业务组件实现。

#if !defined(PROTOSCOPE_WAVE_RENDERER_COMPONENT_INCLUDE)
#error "This wave component implementation is included by wave_dock_renderer.cpp"
#endif


struct WaveFrameData {
    plot::WaveSnapshot snapshot;
    const plot::WaveSnapshot* fullSnapshot{nullptr};
    const plot::WaveDisplayData* displayData{nullptr};
    plot::WaveDataBounds displayBounds{};
    RenderBudget renderBudget;
};

struct PlotRenderResult {
    bool plotRendered{false};
    std::array<std::optional<plot::CursorReadout>, 2> cursorReadouts{};
    std::optional<plot::MeasurementReadout> measurement;
};

void drawMeasurementOverlay(const plot::WaveViewState& view,
                            const plot::WaveSnapshot& snapshot,
                            const plot::WaveDisplayData& displayData,
                            const PlotRenderResult& result);

void initializeWaveViewIfNeeded(plot::WaveViewState& view) {
    if (view.initialized) {
        return;
    }
    view.visibleDuration = (std::max)(view.visibleDuration, (std::max)(view.minVisibleTimeSpan, 1e-6));
    const double halfDuration = view.visibleDuration * 0.5;
    view.viewMinTime = view.centerTime - halfDuration;
    view.viewMaxTime = view.centerTime + halfDuration;
    view.viewMinValue = view.manualVerticalMin;
    view.viewMaxValue = view.manualVerticalMax;
    view.initialized = true;
}

WaveFrameData prepareWaveFrame(plot::WaveDockState& wave, float availableWidth) {
    auto& view = wave.view;
    const double minVisibleTimeSpan = (std::max)(view.minVisibleTimeSpan, 1e-6);

    WaveFrameData frame;
    const auto dataRevision = wave.buffer.dataRevision();
    if (wave.displayDataRevision != dataRevision || wave.displayDataSampleFrequencyHz != view.sampleFrequencyHz) {
        // 核心流程：全量显示数据只在采样数据或时间轴配置变化时重建，避免拖动视图时每帧复制全历史样本。
        wave.cachedFullSnapshot = wave.buffer.snapshot(-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity());
        wave.cachedDisplayData = plot::buildDisplayData(wave.cachedFullSnapshot, view.sampleFrequencyHz);
        wave.cachedDisplayBounds = plot::computeDisplayBounds(wave.cachedDisplayData, minVisibleTimeSpan);
        wave.displayDataRevision = dataRevision;
        wave.displayDataSampleFrequencyHz = view.sampleFrequencyHz;
    }

    frame.fullSnapshot = &wave.cachedFullSnapshot;
    frame.displayData = &wave.cachedDisplayData;
    frame.displayBounds = wave.cachedDisplayBounds;
    view.timeAxisSource = frame.displayData->axisSource;
    frame.renderBudget = makeRenderBudget(view,
                                          frame.displayData->channels.size(),
                                          static_cast<std::size_t>((std::max)(availableWidth, 64.0F)),
                                          view.phosphorGlowEnabled);
    view.lastRenderPointCount = 0;
    view.lastRenderSourceSampleCount = 0;
    view.visibleDuration = (std::max)(view.visibleDuration, minVisibleTimeSpan);
    if (frame.displayBounds.valid && view.autoFollowLatest) {
        view.viewMaxTime = frame.displayBounds.maxTime;
        view.viewMinTime = view.viewMaxTime - view.visibleDuration;
        if (view.viewMinTime < frame.displayBounds.minTime) {
            view.viewMinTime = frame.displayBounds.minTime;
            view.viewMaxTime = (std::min)(frame.displayBounds.maxTime, view.viewMinTime + view.visibleDuration);
        }
        view.centerTime = 0.5 * (view.viewMinTime + view.viewMaxTime);
    }

    frame.snapshot = wave.buffer.snapshot(view.viewMinTime, view.viewMaxTime);
    return frame;
}

void drawCursorToolbar(plot::WaveViewState& view,
                       const plot::ViewConfig& config,
                       const plot::WaveDisplayData& displayData) {
    ImGui::Text("时间轴: %s (%s)", axisSourceName(view.timeAxisSource), displayData.timeUnit.c_str());
    if (!view.showCursors || displayData.channels.empty()) {
        return;
    }
    clampActiveChannel(view, displayData.channels.size());
    const double cursorSnapDistance = (std::max)(view.viewMaxTime - view.viewMinTime, config.timeScale) / 80.0;
    auto placeCursorInViewport = [&](std::size_t cursorIndex, double ratio) {
        auto& cursor = view.cursors[cursorIndex];
        cursor.enabled = true;
        cursor.time = plot::cursorTimeInViewport(currentViewport(view), ratio);
        cursor.channelIndex = view.measurementChannelIndex;
        const auto best = findNearestDisplayByScope(displayData, view, cursor.time, cursorSnapDistance);
        if (best.has_value()) {
            cursor.time = best->time;
            cursor.value = best->value;
            cursor.channelIndex = best->channelIndex;
        }
    };
    if (ImGui::Button("C1 到视窗")) {
        // 快捷定位只移动游标本身，不改变当前视窗，便于快速重取测量区间。
        placeCursorInViewport(0, 0.5);
    }
    ImGui::SameLine();
    if (ImGui::Button("C2 到视窗")) {
        placeCursorInViewport(1, 0.5);
    }
    ImGui::SameLine();
    if (ImGui::Button("C1+C2 到视窗")) {
        placeCursorInViewport(0, 0.4);
        placeCursorInViewport(1, 0.6);
        view.lockedCursorInterval = std::abs(view.cursors[1].time - view.cursors[0].time);
    }
}

void applyMainPlotAxesAndLimits(plot::WaveViewState& view,
                                const plot::WaveSnapshot& snapshot,
                                const plot::WaveDisplayData& displayData) {
    constexpr ImPlotAxisFlags axisFlags = ImPlotAxisFlags_NoHighlight;
    const char* xAxisLabel = nullptr;
    const char* yAxisLabel = nullptr;
    if (view.showAxisLabels) {
        xAxisLabel = displayData.timeUnit == "sample" ? "Sample" : "Time";
        yAxisLabel = snapshot.config.verticalUnit.c_str();
    }
    ImPlot::SetupAxis(ImAxis_X1, xAxisLabel, axisFlags);
    ImPlot::SetupAxis(ImAxis_Y1, yAxisLabel, axisFlags);
    const bool forceMainPlotLimits = view.forceNextMainPlotLimits;
    ImPlot::SetupAxisLimits(ImAxis_X1,
                            view.viewMinTime,
                            view.viewMaxTime,
                            (view.autoFollowLatest || forceMainPlotLimits) ? ImPlotCond_Always : ImPlotCond_Once);
    view.forceNextMainPlotLimits = false;
    if (view.lockVerticalRange) {
        ImPlot::SetupAxisLimits(ImAxis_Y1, view.manualVerticalMin, view.manualVerticalMax, ImPlotCond_Always);
    } else {
        ImPlot::SetupAxisLimits(ImAxis_Y1,
                                view.viewMinValue,
                                view.viewMaxValue,
                                forceMainPlotLimits ? ImPlotCond_Always : ImPlotCond_Once);
    }
}

bool handleMainPlotZoom(plot::WaveViewState& view, const ImPlotPoint& mousePos) {
    const auto& io = ImGui::GetIO();
    if (io.MouseWheel == 0.0F
        || (!ImPlot::IsPlotHovered() && !ImPlot::IsAxisHovered(ImAxis_X1) && !ImPlot::IsAxisHovered(ImAxis_Y1))) {
        return false;
    }

    plot::WaveZoomMode zoomMode =
        view.controlMode == plot::WaveControlMode::Oscilloscope ? plot::WaveZoomMode::XOnly : plot::WaveZoomMode::XY;
    if (ImPlot::IsAxisHovered(ImAxis_X1)) {
        zoomMode = plot::WaveZoomMode::XOnly;
    } else if (ImPlot::IsAxisHovered(ImAxis_Y1)) {
        if (view.controlMode == plot::WaveControlMode::Oscilloscope) {
            return false;
        }
        zoomMode = plot::WaveZoomMode::YOnly;
    }
    const double minVisibleTimeSpan = (std::max)(view.minVisibleTimeSpan, 1e-6);
    const plot::WaveDataBounds bounds{
        .minTime = -std::numeric_limits<double>::infinity(),
        .maxTime = std::numeric_limits<double>::infinity(),
        .minValue = -std::numeric_limits<double>::infinity(),
        .maxValue = std::numeric_limits<double>::infinity(),
        .minStep = minVisibleTimeSpan,
        .valid = false,
    };
    const auto zoomed = plot::zoomViewport(currentViewport(view),
                                           zoomMode,
                                           io.MouseWheel,
                                           mousePos.x,
                                           mousePos.y,
                                           bounds,
                                           minVisibleTimeSpan,
                                           false);
    applyViewport(view, zoomed);
    return true;
}
