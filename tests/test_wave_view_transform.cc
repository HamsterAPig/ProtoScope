#include "../src/ui/wave/wave_render_service.hpp"
#include "test_registry.hpp"

#include <cmath>
#include <stdexcept>

namespace {
void check(bool value, const char* message)
{
    if (!value) {
        throw std::runtime_error(message);
    }
}
}

void test_wave_channel_affine_transform()
{
    using namespace protoscope;
    for (auto formula : {plot::WaveDisplayFormula::ScaleThenOffset, plot::WaveDisplayFormula::OffsetThenScale}) {
        plot::ChannelSpec spec{.ratio = 3.0, .scale = -2.0, .offset = 4.0};
        const auto updated = ui::channelDisplayAffineTransform(spec, formula, 0.5, 7.0);
        check(updated.has_value(), "有效仿射变换应可表达");
        for (double raw : {-4.0, 0.0, 9.0}) {
            const auto display = [&](const plot::ChannelSpec& item) {
                return formula == plot::WaveDisplayFormula::ScaleThenOffset
                           ? raw * item.ratio * item.scale + item.offset
                           : (raw * item.ratio + item.offset) * item.scale;
            };
            check(std::abs(display(*updated) - (0.5 * display(spec) + 7.0)) < 1e-10,
                  "两种显示公式均须满足目标仿射变换");
        }
        check(updated->scale < 0.0 && updated->ratio == spec.ratio, "保留方向与 ratio");
    }
    plot::ChannelSpec zero{.scale = 0.0};
    check(!ui::channelDisplayAffineTransform(zero, plot::WaveDisplayFormula::OffsetThenScale, 1.0, 2.0),
          "零 scale 不得伪造平移");
    plot::WaveDockState wave;
    wave.buffer.configureChannels(3);
    wave.buffer.setChannelSpec(0, {.label = "A", .scale = -2.0});
    wave.buffer.setChannelSpec(1, {.label = "B", .scale = 0.0});
    wave.buffer.append(0, {.samples = {{0.0, 1.0}, {1.0, 2.0}}});
    wave.buffer.append(1, {.samples = {{0.0, 1.0}, {1.0, 2.0}}});
    const plot::WaveViewport baseline{.minTime = 0.0, .maxTime = 1.0, .minValue = -2.0, .maxValue = 2.0};
    wave.view.viewMinValue = 0.0;
    wave.view.viewMaxValue = 2.0;
    check(ui::commitWaveVerticalViewport(wave, baseline, {0, 1, 2}), "框选 Y 范围应提交参数");
    check(wave.buffer.channelSpec(0)->scale == -4.0 && wave.buffer.channelSpec(0)->offset == 0.5,
          "缩放和平移应同时写入参数");
    check(wave.view.viewMinValue == -2.0 && wave.view.viewMaxValue == 2.0, "Y 基准保持固定");
    check(wave.buffer.channelSpec(1)->scale == 0.0 && wave.buffer.channelSpec(1)->offset == 0.0 &&
              !wave.statusMessage.empty(), "无法表达的零 scale 平移应保持参数并提示");
    check(wave.buffer.channelSpec(2)->scale == 1.0, "空通道不参与操作");
    wave.view.lockVerticalRange = true;
    wave.view.viewMinValue = 0.0;
    check(!ui::commitWaveVerticalViewport(wave, baseline, {0}), "锁定轴时不得修改参数");
}

void test_wave_layout_multiframe_parameter_roundtrip()
{
    using namespace protoscope;
    struct Context {
        Context()
        {
            ImGui::CreateContext();
            ImPlot::CreateContext();
            auto& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = ImVec2(1000, 800);
            io.DeltaTime = 1.0F / 60.0F;
            unsigned char* pixels;
            int width, height;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        }
        ~Context() { ImPlot::DestroyContext(); ImGui::DestroyContext(); }
    } context;
    for (auto formula : {plot::WaveDisplayFormula::ScaleThenOffset, plot::WaveDisplayFormula::OffsetThenScale}) {
        plot::WaveDockState wave;
        wave.buffer.configureChannels(3);
        auto config = wave.buffer.viewConfig();
        config.displayFormula = formula;
        wave.buffer.setViewConfig(config);
        wave.view.displayFormula = formula;
        wave.view.initialized = true;
        wave.view.defaultViewportPending = false;
        wave.view.autoFollowLatest = false;
        wave.view.viewMinTime = 0.0;
        wave.view.viewMaxTime = 1.0;
        wave.view.viewMinValue = -2.0;
        wave.view.viewMaxValue = 2.0;
        wave.view.interactionAnimationEnabled = false;
        wave.buffer.setChannelSpec(0, {.label = "A", .scale = -2.0, .offset = 100.0});
        wave.buffer.setChannelSpec(1, {.label = "B", .offset = 10.0});
        wave.buffer.setChannelSpec(2, {.label = "hidden", .scale = 7.0, .offset = 8.0});
        wave.hiddenChannelIndices = {2};
        wave.legendVisibilityRestorePending = true;
        wave.buffer.append(0, {.samples = {{0.0, -100.0}, {1.0, 100.0}}});
        wave.buffer.append(1, {.samples = {{0.0, 42.0}, {1.0, 42.0}}});
        wave.buffer.append(2, {.samples = {{0.0, -1.0}, {1.0, 1.0}}});
        const auto draw = [&](std::optional<plot::WaveViewMode> toolbarMode = std::nullopt) {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(950, 750));
            ImGui::Begin("wave regression", nullptr, ImGuiWindowFlags_NoSavedSettings);
            auto frame = ui::prepareWaveFrame(wave, 900.0F);
            if (toolbarMode) {
                ui::setWaveViewMode(wave.view, *toolbarMode);
            }
            auto result = ui::drawOscilloscopePlot(wave, frame, {.drawMeasurementOverlay = false,
                                                               .drawLegendOverlay = false}, nullptr);
            check(frame.snapshot.channels[0].scale == wave.buffer.channelSpec(0)->scale &&
                      frame.snapshot.channels[0].offset == wave.buffer.channelSpec(0)->offset,
                  "布局更新必须回传共享帧，覆盖层不能混用旧元数据");
            ImGui::End();
            ImGui::Render();
            check(result.plotRendered, "真实 ImPlot 应成功绘制");
            check(ImGui::GetDrawData()->TotalVtxCount > 0, "绘制应生成顶点");
        };
        draw();
        draw(plot::WaveViewMode::Stacked);
        const auto stacked = *wave.buffer.channelSpec(0);
        check(std::abs(stacked.scale + 0.005) < 1e-12, "堆叠波幅应写入真实 scale");
        check(std::abs(wave.buffer.channelSpec(1)->offset + 40.4) < 1e-10,
              "常量应居中到第二行");
        draw();
        check(wave.buffer.channelSpec(0)->scale == stacked.scale, "普通帧不得重复归一化");
        draw(plot::WaveViewMode::Split);
        const auto split = *wave.buffer.channelSpec(0);
        check(std::abs(split.scale + 0.016) < 1e-12, "分屏应适配固定 Y 基准");
        check(wave.view.viewMinValue == -2.0 && wave.view.viewMaxValue == 2.0, "分屏不得覆盖普通 Y 基准");
        draw();
        draw(plot::WaveViewMode::Overlay);
        check(wave.buffer.channelSpec(0)->scale == split.scale, "返回叠加保留真实参数");
        for (int iteration = 0; iteration < 3; ++iteration) {
            wave.view.fitVisibleWaveformsRequested = true;
            draw();
            check(std::abs(wave.buffer.channelSpec(0)->scale - split.scale) < 1e-12, "重复适配不得漂移");
        }
        check(wave.buffer.channelSpec(2)->scale == 7.0 && wave.buffer.channelSpec(2)->offset == 8.0,
              "隐藏通道参数不变");
        check(wave.buffer.snapshot(0, 1).channels[0].samples[0].value == -100.0, "原始采样不变");
    }
}
