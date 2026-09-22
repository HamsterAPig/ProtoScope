#include "../src/ui/runtime/gui_runtime_detail.hpp"

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
}

int main()
{
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1000, 800);
    io.DeltaTime = 1.0F / 60.0F;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    int status = 0;
    try {
        std::vector<protoscope::dock::ReceiveRow> storage(3000);
        for (auto& row : storage) {
            row.direction = "RX";
            row.endpoint = "COM1";
            row.bytes = {0x41, 0x42, 0x43};
        }
        std::vector<const protoscope::dock::ReceiveRow*> rows;
        bool paused = false;
        ImGuiWindow* child = nullptr;
        float rowStep = 0;
        auto render = [&](int count, bool hex, ImVec2 size) {
            rows.clear();
            for (int i = 0; i < count; ++i) rows.push_back(&storage[i]);
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(size);
            ImGui::Begin("log regression", nullptr, ImGuiWindowFlags_NoSavedSettings);
            if (rowStep == 0) {
                const auto start = ImGui::GetCursorPosY();
                protoscope::ui::drawModernLogRow(storage[0], true, hex, 0, 60);
                rowStep = ImGui::GetCursorPosY() - start;
                ImGui::SetCursorPosY(start);
            }
            protoscope::ui::drawModernLogRows("records", rows, true, hex, paused, "empty", 60);
            child = protoscope::ui::findMultilineChildWindow("records");
            ImGui::End();
            ImGui::Render();
            require(child != nullptr, "log child missing");
        };
        for (const auto size : {ImVec2(850, 600), ImVec2(420, 320)}) {
            for (bool hex : {false, true}) {
                paused = false;
                for (int n = 0; n < 12; ++n) {
                    render(1000, hex, size);
                    ImGui::SetScrollY(child, child->ScrollMax.y);
                }
                const float baselineY = child->Scroll.y;
                const float baselineHeight = child->ContentSize.y;
                std::cout << "row step=" << rowStep << " height=" << baselineHeight
                          << " scroll=" << baselineY << '\n';
                require(std::abs(baselineHeight - (1000 * rowStep - ImGui::GetStyle().ItemSpacing.y)) <= 1,
                        "clipper content height disagrees with actual row stride");
                for (int n = 0; n < 120; ++n) {
                    render(1000, hex, size);
                    require(child->Scroll.y == baselineY && child->ContentSize.y == baselineHeight,
                            "stopped log oscillates at bottom");
                }
                for (int n = 0; n < 120; ++n) render(1001 + n, hex, size);
                for (int n = 0; n < 6; ++n) render(1120, hex, size);
                require(std::abs(child->Scroll.y - child->ScrollMax.y) <= 1, "append lost bottom following");
                paused = true;
                const auto pausedY = child->Scroll.y;
                for (int n = 0; n < 20; ++n) render(1200 + n, hex, size);
                require(child->Scroll.y == pausedY, "paused log followed append");
                paused = false;
                ImGui::SetScrollY(child, 100);
                for (int n = 0; n < 20; ++n) render(1300 + n, hex, size);
                require(std::abs(child->Scroll.y - 100) <= 1, "manual scroll was stolen");
            }
        }
        // 长记录、过滤缩短以及不同日志类别都复用相同固定行布局。
        for (const char* direction : {"RX", "TX", "INFO", "SCRIPT"}) {
            for (auto& row : storage) {
                row.direction = direction;
                row.message = std::string(500, 'x');
            }
            for (int n = 0; n < 15; ++n) render(40, false, ImVec2(500, 400));
            ImGui::SetScrollY(child, child->ScrollMax.y);
            for (int n = 0; n < 12; ++n) render(40, false, ImVec2(500, 400));
            const float y = child->Scroll.y;
            const float contentHeight = child->ContentSize.y;
            for (int n = 0; n < 120; ++n) {
                render(40, false, ImVec2(500, 400));
                require(child->Scroll.y == y && child->ContentSize.y == contentHeight,
                        "filtered long log geometry unstable");
            }
            require(child->ScrollbarX, "long log should have horizontal scrollbar");
        }
        std::cout << "Log scroll: 120-frame stability, append, pause, manual scroll, HEX, resize and long rows passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        status = 1;
    }
    ImGui::DestroyContext();
    return status;
}
