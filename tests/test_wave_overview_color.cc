#include "../src/ui/wave/wave_render_service.hpp"
#include "protoscope/plot/wave_overview_color.hpp"
#include "protoscope/ui/ui_theme.hpp"

#include <iostream>
#include <stdexcept>

using namespace protoscope;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void verifyChoice(std::span<const plot::OverviewColor> colors, plot::OverviewColor background)
{
    const auto selected = plot::selectOverviewColor(colors, background);
    require(std::ranges::find(plot::kOverviewPalette, selected) != plot::kOverviewPalette.end(), "non-palette color");
    require(plot::overviewContrast(plot::overviewRgb(selected), background) >= 3, "border contrast below 3:1");
    for (const auto candidate : plot::kOverviewPalette)
        if (plot::overviewContrast(plot::overviewRgb(candidate), background) >= 3)
            require(plot::overviewColorScore(plot::overviewRgb(selected), colors, background) >=
                    plot::overviewColorScore(plot::overviewRgb(candidate), colors, background), "not best contrast score");
}

int main()
{
    ImGui::CreateContext();
    ImPlot::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(900, 400);
    io.DeltaTime = 1.0F / 60;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    int status = 0;
    try {
        {
            const std::array backgrounds{plot::OverviewColor{.02, .03, .04, 1}};
            const std::array labels{plot::OverviewColor{.08, .09, .1, 1}};
            const std::array waves{plot::overviewRgb(0x66CCEE), plot::overviewRgb(0xCCBB44)};
            const auto choice = plot::selectCursorColor(waves, backgrounds, labels);
            require(choice.graphicsPass && choice.textPass, "游标背景图形/文字阈值");
            require(choice.color == plot::selectCursorColor(waves, backgrounds, labels).color, "候选平局不确定");
            const std::array impossible{plot::OverviewColor{0, 0, 0, 1}, plot::OverviewColor{.5, .5, .5, 1},
                                        plot::OverviewColor{1, 1, 1, 1}};
            require(!plot::selectCursorColor(waves, impossible, impossible).textPass, "无解必须报告文字未达标");
            plot::CursorColorKey key;
            key.backgrounds.assign(backgrounds.begin(), backgrounds.end());
            key.textBackgrounds.assign(labels.begin(), labels.end());
            key.colors.assign(waves.begin(), waves.end());
            key.channels = {0, 1};
            key.manualPalette = {plot::OverviewColor{.2, .4, .6, .7}, plot::OverviewColor{.7, .4, .2, 1}};
            plot::CursorColorCache cursors;
            cursors.prepare(key, 1, false);
            const auto a = cursors.resolve(0, 0).color;
            const auto b = cursors.resolve(1, 1).color;
            const auto t = cursors.resolve(3, 2).color;
            require(a != b && a != t && b != t, "A/B/T 色槽必须区分");
            for (std::uint64_t id = 4; id < 40; ++id) cursors.resolve(id, 3);
            require(cursors.resolve(3, 4).color == t, "新增及候选耗尽不得重排已有 T");
            auto changed = key;
            changed.colors = {a};
            cursors.prepare(changed, 1, false);
            require(cursors.key == key, "同帧必须冻结");
            cursors.prepare(changed, 2, true);
            require(cursors.key == key && cursors.resolve(0, 0).color == a, "拖动必须锁色");
            cursors.prepare(changed, 3, false);
            require(cursors.key == changed && cursors.updates == 2, "释放必须发布待更新颜色");
            changed.themeRevision = 7;
            cursors.prepare(changed, 4, true);
            require(cursors.key.themeRevision == 7, "主题变更不得被拖动锁屏蔽");
            changed.automatic = false;
            cursors.prepare(changed, 5, false);
            require(cursors.resolve(0, 0).color == key.manualPalette[0] &&
                    cursors.resolve(3, 3).color == key.manualPalette[1], "关闭自动色必须保留手动 RGBA/索引");
        }
        const auto white = plot::overviewLab({1, 1, 1, 1});
        const auto black = plot::overviewLab({0, 0, 0, 1});
        require(std::abs(white[0] - 100) < 1e-4 && std::abs(white[1]) < 1e-3 &&
                std::abs(white[2]) < 1e-3 && black[0] == 0, "D65 Lab reference conversion");
        const auto red = plot::overviewLab({1, 0, 0, 1});
        require(std::abs(red[0] - 53.2408) < 0.001 && std::abs(red[1] - 80.0925) < 0.001 &&
                std::abs(red[2] - 67.2032) < 0.001, "sRGB red reference conversion");
        for (const auto bg : {plot::OverviewColor{0.03, 0.04, 0.05, 1}, plot::OverviewColor{1, 1, 1, 1}}) {
            verifyChoice({}, bg);
            for (const auto rgb : plot::kOverviewPalette) {
                const std::array colors{plot::overviewRgb(rgb)};
                verifyChoice(colors, bg);
            }
            const std::array colors{plot::OverviewColor{0.8, 0.2, 0.7, 0.2},
                                    plot::OverviewColor{0.3, 0.9, 0.6, 0.65},
                                    plot::OverviewColor{0.1, 0.3, 0.9, 1}};
            verifyChoice(colors, bg);
            std::array<plot::OverviewColor, 3> opaque;
            for (std::size_t i = 0; i < colors.size(); ++i)
                opaque[i] = plot::compositeOverviewColor(colors[i], bg);
            require(plot::selectOverviewColor(colors, bg) == plot::selectOverviewColor(opaque, bg),
                    "alpha not composited against actual background");
        }
        plot::WaveDockState wave;
        wave.view.initialized = true;
        wave.view.defaultViewportPending = false;
        wave.view.autoFollowLatest = false;
        wave.view.viewMinTime = 0.2;
        wave.view.viewMaxTime = 0.6;
        wave.buffer.setChannelSpec(0, {.color = std::array{0.9F, 0.2F, 0.1F, 0.5F}});
        wave.buffer.append(0, {{}, {{0, 0}, {1, 1}}});
        wave.buffer.append(1, {{}, {{0, 1}, {1, 0}}});
        const auto render = [&] {
            ImGui::NewFrame();
            ImGui::SetNextWindowSize(ImVec2(880, 380));
            ImGui::Begin("Overview color verification");
            auto frame = ui::prepareWaveFrame(wave, 800);
            ui::drawOverviewWindow(wave, frame.fullSnapshot->config, *frame.fullSnapshot,
                *frame.overviewDisplayData, plot::computeDisplayBounds(*frame.overviewDisplayData, 1e-6),
                {0, 1}, frame.renderBudget);
            ImGui::End();
            ImGui::Render();
        };
        for (const auto theme : {config::GuiTheme::ProfessionalDark, config::GuiTheme::DebugHighContrast,
                                 config::GuiTheme::ProfessionalLight}) {
            ui::applyUiTheme(theme);
            wave.buffer.setChannelSpec(1, {});
            wave.hiddenChannelIndices.clear();
            render();
            render();
            const auto colorUpdates = wave.view.cursorColors.updates;
            const auto aColor = ui::measurementCursorColor(wave.view, 0);
            render();
            require(wave.view.cursorColors.updates == colorUpdates, "采样/鼠标未变化不应重选游标色");
            require(ui::cursorOverviewColor(aColor) == ui::cursorOverviewColor(ui::measurementCursorColor(wave.view, 0)),
                    "帧间游标身份不稳定");
            require(wave.overviewColorCache.channels.size() == 2, "visible channel color missing");
            require(wave.overviewColorCache.channels[0].a >= .325 - 1e-6, "overview alpha lost");
            require(wave.overviewColorCache.alpha <= .28, "overview alpha cap");
            const auto updates = wave.overviewColorCache.updates;
            const auto color = wave.overviewColorCache.selected;
            for (int n = 0; n < 8; ++n) {
                wave.buffer.append(1, {{}, {{2.0 + n, double(n % 2)}}});
                render();
            }
            require(wave.overviewColorCache.updates == updates && wave.overviewColorCache.selected == color,
                    "data refresh recomputed or flashed rectangle color");
            wave.hiddenChannelIndices = {0};
            render();
            require(wave.overviewColorCache.channels.size() == 1 &&
                    wave.overviewColorCache.updates == updates + 1, "hidden channel still affects selection");
            wave.buffer.setChannelSpec(1, {.color = std::array{0.3F, 0.2F, 0.8F, 0.8F}});
            render();
            require(wave.overviewColorCache.updates == updates + 2, "custom color did not invalidate selection");
            const auto rgb = wave.overviewColorCache.selected;
            const ImU32 border = IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, 255);
            bool found = false;
            for (const auto* list : ImGui::GetDrawData()->CmdLists)
                for (const auto& vertex : list->VtxBuffer) found = found || vertex.col == border;
            require(found, "rectangle border is not opaque palette color");
            wave.view.overviewSelection.automatic = false;
            wave.view.overviewSelection.fixedColor = std::array{.5F, .25F, .75F};
            wave.view.overviewSelection.minAlpha = .05F;
            wave.view.overviewSelection.maxAlpha = .08F;
            wave.view.viewMaxTime = wave.view.viewMinTime + 1e-9;
            wave.hiddenChannelIndices = {0, 1};
            render();
            require(wave.overviewColorCache.alpha <= .08F, "fixed alpha maximum");
            require(wave.overviewColorCache.channels.empty(), "all hidden channels must not contribute");
            wave.view.overviewSelection = {};
            wave.view.viewMaxTime = .6;
        }
        wave.hiddenChannelIndices.clear();
        wave.buffer.setChannelSpec(0, {.color = std::array{1.F, 0.F, 0.F, 0.F}});
        render();
        require(std::none_of(wave.view.cursorColors.key.channels.begin(), wave.view.cursorColors.key.channels.end(),
                            [](std::size_t identity) { return identity / 8 == 0; }), "全透明通道进入游标选色");
        const auto oldKey = wave.view.cursorColors.key;
        io.AddMouseButtonEvent(0, true);
        wave.hiddenChannelIndices = {1};
        render();
        require(wave.view.cursorColors.key == oldKey, "拖动期间显隐改变了游标色键");
        io.AddMouseButtonEvent(0, false); render();
        require(wave.view.cursorColors.key.colors.empty(), "隐藏通道进入游标选色");
        wave.view.cursorAutoColor = false;
        render();
        const auto& tokens = ui::activeWaveStyleTokens();
        const auto expected = ui::displayColor(ui::displayColor(tokens.cursorPalette[0], tokens.plotBackground, 1.F, 4.5F),
                                               ui::activeUiStyleTokens().panelBackgroundAlt, 1.F, 4.5F);
        require(ui::cursorOverviewColor(ui::measurementCursorColor(wave.view, 0)) == ui::cursorOverviewColor(expected),
                "手动路径未恢复原主题及 correct_contrast 语义");
        plot::OverviewColorRaster raster({1,1,1,1});
        raster.rectangle(0, 0, 1, 1, {1,0,0,.5});
        raster.rectangle(0, 0, 1, 1, {0,0,1,.5});
        require(raster.pixels[0] == plot::OverviewColor{.5,.25,.75,1}, "raster draw order/alpha");
        plot::OverviewColorCache cache;
        const plot::OverviewColor bg{.02,.02,.02,1};
        plot::OverviewSelectionStyle settings;
        std::array sample{plot::OverviewColor{.8,.2,.5,1}};
        cache.resolveSamples({}, bg, sample, settings, 1, 0, 0, .016);
        const auto initial = cache.selected;
        const auto count = cache.updates;
        sample[0] = plot::overviewRgb(initial);
        cache.resolveSamples({}, bg, sample, settings, 1, 0, .1, .016);
        require(cache.updates == count, "evaluation throttling");
        cache.dragging = true;
        cache.resolveSamples({}, bg, sample, settings, 1, 0, 2, .016);
        require(cache.selected == initial && cache.updates == count, "drag locks color");
        cache.dragging = false;
        cache.resolveSamples({}, bg, sample, settings, 1, 0, 2.1, .016);
        require(cache.selected == initial, "candidate changed without hysteresis");
        cache.resolveSamples({}, bg, sample, settings, 1, 0, 2.31, .016);
        require(cache.selected == initial, "candidate changed before 400ms");
        cache.resolveSamples({}, bg, sample, settings, 1, 0, 2.52, .016);
        require(cache.selected != initial, "persistent improved candidate not adopted");
        std::cout << "overview_color: palette, Lab, alpha, contrast, visibility, themes and cache passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        status = 1;
    }
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    return status;
}
