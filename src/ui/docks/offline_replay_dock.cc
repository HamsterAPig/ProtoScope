#include "protoscope/app/application.hpp"
#include "protoscope/ui/gui_runtime.hpp"
#include "protoscope/ui/ui_theme.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iterator>
#include <string>

#include <imgui.h>

namespace protoscope::ui {

namespace {

    std::string formatBytes(const std::uint64_t bytes)
    {
        static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB"};
        double value = static_cast<double>(bytes);
        std::size_t unitIndex = 0;
        while (value >= 1024.0 && unitIndex + 1U < std::size(kUnits)) {
            value /= 1024.0;
            ++unitIndex;
        }

        char buffer[64]{};
        if (unitIndex == 0U) {
            std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(bytes));
        } else {
            std::snprintf(buffer, sizeof(buffer), "%.2f %s", value, kUnits[unitIndex]);
        }
        return buffer;
    }

    std::string formatCount(const std::size_t count)
    {
        char buffer[48]{};
        std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(count));
        return buffer;
    }

    std::string dashIfEmpty(const std::string& value)
    {
        return value.empty() ? std::string("-") : value;
    }

    std::string fileNameOrDash(const std::string& pathText)
    {
        if (pathText.empty()) {
            return "-";
        }
        const auto path = std::filesystem::path(pathText);
        const auto name = path.filename().generic_string();
        return name.empty() ? path.generic_string() : name;
    }

    void drawSectionTitle(const char* title)
    {
        ImGui::Spacing();
        ImGui::TextUnformatted(title);
        ImGui::Separator();
    }

    void drawKeyValue(const char* key, const std::string& value)
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", key);
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", value.c_str());
    }

    void drawKeyValue(const char* key, const char* value)
    {
        drawKeyValue(key, std::string(value));
    }

    bool drawDisabledAwareButton(const char* label, const char* tooltip, const bool enabled)
    {
        if (!enabled) {
            ImGui::BeginDisabled();
        }
        const bool clicked = drawToolbarSectionButton(label, tooltip, false, ImVec2(-1.0F, 0.0F));
        if (!enabled) {
            ImGui::EndDisabled();
        }
        return clicked && enabled;
    }

    void drawTwoColumnActionRow(const char* leftLabel,
                                const char* leftTooltip,
                                const bool leftEnabled,
                                const char* rightLabel,
                                const char* rightTooltip,
                                const bool rightEnabled,
                                const std::function<void()>& leftAction,
                                const std::function<void()>& rightAction)
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (drawDisabledAwareButton(leftLabel, leftTooltip, leftEnabled)) {
            leftAction();
        }
        ImGui::TableNextColumn();
        if (drawDisabledAwareButton(rightLabel, rightTooltip, rightEnabled)) {
            rightAction();
        }
    }

    const char* replayStateLabel(const app::Application::RawCaptureReplayStatus& status)
    {
        if (!status.loaded) {
            return "未载入";
        }
        if (status.playing) {
            return "播放中";
        }
        if (status.eventIndex >= status.eventCount) {
            return "已结束";
        }
        return "已暂停";
    }

    std::string replayPositionText(const app::Application::RawCaptureReplayStatus& status)
    {
        char buffer[96]{};
        std::snprintf(buffer,
                      sizeof(buffer),
                      "%llu / %llu (%.1f%%)",
                      static_cast<unsigned long long>(status.eventIndex),
                      static_cast<unsigned long long>(status.eventCount),
                      status.progress * 100.0);
        return buffer;
    }

} // namespace

void GuiRuntime::drawOfflineReplayDock()
{
    if (!showOfflineReplayDock_) {
        return;
    }

    if (!ImGui::Begin("离线复现", &showOfflineReplayDock_)) {
        ImGui::End();
        return;
    }

    auto& docks = application_.docks();
    const auto& lua = docks.luaState();
    const auto& wave = docks.waveState();
    const auto& capture = wave.rawCapture;
    const auto replayStatus = application_.rawCaptureReplayStatus();
    const bool canAdvanceReplay = replayStatus.loaded && replayStatus.eventIndex < replayStatus.eventCount;

    drawSectionTitle("复现输入");
    if (ImGui::BeginTable("##offline_replay_inputs", 2, ImGuiTableFlags_SizingStretchSame)) {
        drawTwoColumnActionRow(
            "导入现场包",
            "导入 .pssession 现场会话包，恢复协议与复现上下文，并在时间轴起点暂停。",
            true,
            "导出现场包",
            "导出 .pssession 现场会话包，打包当前协议和复现证据。",
            true,
            [&]() { openSessionPackageImportDialog(); },
            [&]() { openSessionPackageExportDialog(); });
        drawTwoColumnActionRow(
            "导入缓存快照",
            "导入 .psraw 缓存快照，重建当前可查看的原始波形。",
            true,
            "导出缓存快照",
            "导出当前窗口内的 .psraw 原始字节和必要配置快照。",
            true,
            [&]() { openRawCaptureImportDialog(); },
            [&]() { openRawCaptureExportDialog(); });
        drawTwoColumnActionRow(
            "载入回放时间轴",
            "载入 .psraw 完整事件流，用原始时间戳回放采集过程。",
            true,
            "打开 ELF 数据",
            "打开 ELF/ElfStaticView 数据文件",
            true,
            [&]() { openRawCaptureReplayTimelineDialog(); },
            [&]() { openElfStaticAddressDialog(); });
        drawTwoColumnActionRow(
            "重载当前协议",
            "重新加载当前协议目录",
            !lua.protocolDir.empty(),
            "显示波形 Dock",
            "显示波形 Dock",
            true,
            [&]() { requestProtocolWorkspaceSwitch(lua.protocolDir, true); },
            [&]() {
                showWaveDock_ = true;
                pendingProtocolWorkspaceSave_ = true;
            });
        ImGui::EndTable();
    }

    drawSectionTitle("回放控制");
    if (ImGui::BeginTable("##offline_replay_controls", 3, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (drawDisabledAwareButton("继续回放", "从当前位置继续按原始时间轴播放事件。", canAdvanceReplay && !replayStatus.playing)) {
            std::string error;
            if (!application_.playRawCaptureReplay(error)) {
                application_.setStatusMessage("原始回放继续失败: " + error);
            }
        }
        ImGui::TableNextColumn();
        if (drawDisabledAwareButton("暂停回放", "暂停时间轴播放，保留当前位置。", replayStatus.loaded && replayStatus.playing)) {
            application_.pauseRawCaptureReplay();
        }
        ImGui::TableNextColumn();
        if (drawDisabledAwareButton("单步推进", "只执行下一个原始事件，便于逐帧排查。", canAdvanceReplay)) {
            std::string error;
            if (!application_.stepRawCaptureReplay(error)) {
                application_.setStatusMessage("原始回放单步失败: " + error);
            }
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (drawDisabledAwareButton("开头", "定位到时间轴开头", replayStatus.loaded)) {
            std::string error;
            if (!application_.seekRawCaptureReplay(0, error)) {
                application_.setStatusMessage("原始回放定位失败: " + error);
            }
        }
        ImGui::TableNextColumn();
        if (drawDisabledAwareButton("中点", "定位到时间轴中点", replayStatus.loaded && replayStatus.eventCount > 0U)) {
            std::string error;
            if (!application_.seekRawCaptureReplay(replayStatus.eventCount / 2U, error)) {
                application_.setStatusMessage("原始回放定位失败: " + error);
            }
        }
        ImGui::TableNextColumn();
        if (drawDisabledAwareButton("末尾", "定位到时间轴末尾", replayStatus.loaded && replayStatus.eventCount > 0U)) {
            std::string error;
            if (!application_.seekRawCaptureReplay(replayStatus.eventCount, error)) {
                application_.setStatusMessage("原始回放定位失败: " + error);
            }
        }

        ImGui::TableNextRow();
        static constexpr double kReplaySpeeds[] = {0.5, 1.0, 2.0};
        for (const double speed : kReplaySpeeds) {
            ImGui::TableNextColumn();
            char label[24]{};
            std::snprintf(label, sizeof(label), "%.1fx", speed);
            const bool selected = std::abs(replayStatus.speed - speed) < 0.001;
            if (drawDisabledAwareButton(label, "设置回放倍速", replayStatus.loaded)) {
                application_.setRawCaptureReplaySpeed(speed);
            }
            if (selected && replayStatus.loaded) {
                ImGui::SameLine();
                ImGui::TextDisabled("*");
            }
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (drawDisabledAwareButton("卸载时间轴", "停止回放并释放当前载入的 .psraw 时间轴。", replayStatus.loaded)) {
            application_.unloadRawCaptureReplayTimeline();
        }
        ImGui::TableNextColumn();
        if (drawDisabledAwareButton("请求追踪", "显示请求追踪 Dock", true)) {
            showRequestTraceDock_ = true;
            pendingProtocolWorkspaceSave_ = true;
        }
        ImGui::TableNextColumn();
        if (drawDisabledAwareButton("日志 Dock", "显示日志和脚本 Dock", true)) {
            showLogDock_ = true;
            showScriptDock_ = true;
            pendingProtocolWorkspaceSave_ = true;
        }
        ImGui::EndTable();
    }

    drawSectionTitle("证据导出");
    if (ImGui::BeginTable("##offline_replay_exports", 3, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (drawToolbarSectionButton("收发日志", "导出当前筛选后的收发日志", false, ImVec2(-1.0F, 0.0F))) {
            openTransferLogExportDialog();
        }
        ImGui::TableNextColumn();
        if (drawToolbarSectionButton("宿主日志", "导出当前筛选后的宿主日志", false, ImVec2(-1.0F, 0.0F))) {
            openHostLogExportDialog();
        }
        ImGui::TableNextColumn();
        if (drawToolbarSectionButton("脚本日志", "导出当前筛选后的 Lua 日志", false, ImVec2(-1.0F, 0.0F))) {
            openScriptLogExportDialog();
        }
        ImGui::EndTable();
    }

    drawSectionTitle("当前上下文");
    if (ImGui::BeginTable("##offline_replay_context", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, 120.0F);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        drawKeyValue("协议", dashIfEmpty(lua.protocolName));
        drawKeyValue("协议目录", dashIfEmpty(lua.protocolDir));
        drawKeyValue("协议根目录", dashIfEmpty(lua.protocolRootDir));
        drawKeyValue("ELF 数据", fileNameOrDash(elfStaticAddressPath_));
        drawKeyValue("缓存原始字节", formatBytes(static_cast<std::uint64_t>(capture.payload.size())));
        drawKeyValue("缓存事件", formatCount(capture.events.size()));
        drawKeyValue("缓存来源协议", dashIfEmpty(capture.protocolDir));
        drawKeyValue("缓存截断", capture.truncated ? "是" : "否");
        drawKeyValue("录制状态", application_.isRawCaptureRecording() ? "录制中" : "未录制");
        drawKeyValue("录制文件",
                     application_.rawCaptureRecordingPath().empty()
                         ? std::string("-")
                         : application_.rawCaptureRecordingPath().filename().generic_string());
        drawKeyValue("录制字节", formatBytes(application_.rawCaptureRecordingBytes()));
        drawKeyValue("回放状态", replayStateLabel(replayStatus));
        drawKeyValue("回放位置", replayPositionText(replayStatus));
        drawKeyValue("请求追踪", formatCount(docks.requestTraceState().rows.size()));
        drawKeyValue("收发日志", formatCount(docks.receiveState().rows.size()));
        drawKeyValue("宿主日志", formatCount(docks.logState().rows.size()));
        drawKeyValue("脚本日志", formatCount(docks.scriptState().rows.size()));
        ImGui::EndTable();
    }

    ImGui::End();
}

} // namespace protoscope::ui
