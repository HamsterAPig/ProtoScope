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
    const auto minDistance = [&](std::uint32_t rgb) {
        const auto lab = plot::overviewLab(plot::overviewRgb(rgb));
        double nearest = std::numeric_limits<double>::infinity();
        for (const auto channel : colors) {
            const auto other = plot::overviewLab(plot::compositeOverviewColor(channel, background));
            const auto distance = std::hypot(lab[0] - other[0], lab[1] - other[1], lab[2] - other[2]);
            nearest = (std::min)(nearest, distance);
        }
        return nearest;
    };
    for (const auto candidate : plot::kOverviewPalette)
        if (plot::overviewContrast(plot::overviewRgb(candidate), background) >= 3)
            require(minDistance(selected) >= minDistance(candidate) - 1e-9, "not maximum minimum Lab distance");
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
        for (const auto theme : {config::GuiTheme::ProfessionalDark, config::GuiTheme::DebugHighContrast}) {
            ui::applyUiTheme(theme);
            wave.buffer.setChannelSpec(1, {});
            wave.hiddenChannelIndices.clear();
            render();
            render();
            require(wave.overviewColorCache.channels.size() == 2, "visible channel color missing");
            require(std::abs(wave.overviewColorCache.channels[0].a - 0.325) < 1e-6, "overview alpha lost");
            verifyChoice(wave.overviewColorCache.channels, wave.overviewColorCache.background);
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
            verifyChoice(wave.overviewColorCache.channels, wave.overviewColorCache.background);
            wave.buffer.setChannelSpec(1, {.color = std::array{0.3F, 0.2F, 0.8F, 0.8F}});
            render();
            require(wave.overviewColorCache.updates == updates + 2, "custom color did not invalidate selection");
            const auto rgb = wave.overviewColorCache.selected;
            const ImU32 border = IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, 255);
            bool found = false;
            for (const auto* list : ImGui::GetDrawData()->CmdLists)
                for (const auto& vertex : list->VtxBuffer) found = found || vertex.col == border;
            require(found, "rectangle border is not opaque palette color");
        }
        std::cout << "overview_color: palette, Lab, alpha, contrast, visibility, themes and cache passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        status = 1;
    }
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    return status;
}
