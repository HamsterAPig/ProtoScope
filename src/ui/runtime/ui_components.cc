#include "protoscope/ui/gui_runtime.hpp"
#include "protoscope/ui/ui_component.hpp"

namespace protoscope::ui {

class RuntimeMenuComponent final : public IMenuContributor {
public:
    explicit RuntimeMenuComponent(GuiRuntime& runtime) : runtime_(runtime) {}

    std::string_view id() const override { return "runtime.menu"; }

    void drawMainMenuItems(RuntimeUiContext&) override { runtime_.drawMainMenu(); }

private:
    GuiRuntime& runtime_;
};

class RuntimeDialogComponent final : public IDialogComponent {
public:
    explicit RuntimeDialogComponent(GuiRuntime& runtime) : runtime_(runtime) {}

    std::string_view id() const override { return "runtime.dialogs"; }

    void syncRequests(RuntimeUiContext&) override { runtime_.syncDialogQueue(); }

    void drawDialogs(RuntimeUiContext&) override
    {
        runtime_.drawAboutDialog();
        runtime_.drawShortcutHelpDialog();
        runtime_.drawUpdateCheckDialog();
        runtime_.drawDialogs();
        runtime_.drawRawCaptureFileDialogs();
        runtime_.drawLogExportFileDialog();
        runtime_.drawElfStaticAddressDialog();
    }

private:
    GuiRuntime& runtime_;
};

class CommDockComponent final : public IDockComponent {
public:
    explicit CommDockComponent(GuiRuntime& runtime) : runtime_(runtime) {}

    std::string_view id() const override { return "dock.comm"; }

    std::string_view title() const override { return "通信"; }

    bool defaultVisible() const override { return true; }

    void drawDock(RuntimeUiContext&) override
    {
        runtime_.drawCommDock();
        runtime_.drawTransferDock();
    }

private:
    GuiRuntime& runtime_;
};

class ProtocolDockComponent final : public IDockComponent {
public:
    explicit ProtocolDockComponent(GuiRuntime& runtime) : runtime_(runtime) {}

    std::string_view id() const override { return "dock.protocol"; }

    std::string_view title() const override { return "协议"; }

    bool defaultVisible() const override { return true; }

    void drawDock(RuntimeUiContext&) override { runtime_.drawProtocolDock(); }

private:
    GuiRuntime& runtime_;
};

class LuaDockComponent final : public IDockComponent {
public:
    explicit LuaDockComponent(GuiRuntime& runtime) : runtime_(runtime) {}

    std::string_view id() const override { return "dock.lua"; }

    std::string_view title() const override { return "Lua 控件"; }

    bool defaultVisible() const override { return true; }

    void drawDock(RuntimeUiContext&) override { runtime_.drawLuaDockWindows(); }

private:
    GuiRuntime& runtime_;
};

class RequestTraceDockComponent final : public IDockComponent {
public:
    explicit RequestTraceDockComponent(GuiRuntime& runtime) : runtime_(runtime) {}

    std::string_view id() const override { return "dock.request_trace"; }

    std::string_view title() const override { return "请求追踪"; }

    bool defaultVisible() const override { return true; }

    void drawDock(RuntimeUiContext&) override { runtime_.drawRequestTraceDock(); }

private:
    GuiRuntime& runtime_;
};

class OfflineReplayDockComponent final : public IDockComponent {
public:
    explicit OfflineReplayDockComponent(GuiRuntime& runtime) : runtime_(runtime) {}

    std::string_view id() const override { return "dock.offline_replay"; }

    std::string_view title() const override { return "离线复现"; }

    bool defaultVisible() const override { return true; }

    void drawDock(RuntimeUiContext&) override { runtime_.drawOfflineReplayDock(); }

private:
    GuiRuntime& runtime_;
};

class LogDockComponent final : public IDockComponent {
public:
    explicit LogDockComponent(GuiRuntime& runtime) : runtime_(runtime) {}

    std::string_view id() const override { return "dock.logs"; }

    std::string_view title() const override { return "日志"; }

    bool defaultVisible() const override { return true; }

    void drawDock(RuntimeUiContext&) override
    {
        runtime_.drawLogDock();
        runtime_.drawScriptDock();
    }

private:
    GuiRuntime& runtime_;
};

UiComponentRegistry::ComponentList UiComponentRegistry::createRuntimeComponents(GuiRuntime& runtime)
{
    ComponentList components;
    components.emplace_back(std::make_unique<RuntimeMenuComponent>(runtime));
    components.emplace_back(std::make_unique<RuntimeDialogComponent>(runtime));
    components.emplace_back(std::make_unique<CommDockComponent>(runtime));
    components.emplace_back(std::make_unique<ProtocolDockComponent>(runtime));
    components.emplace_back(std::make_unique<LuaDockComponent>(runtime));
    components.emplace_back(std::make_unique<RequestTraceDockComponent>(runtime));
    components.emplace_back(std::make_unique<OfflineReplayDockComponent>(runtime));
    components.emplace_back(std::make_unique<LogDockComponent>(runtime));
    return components;
}

} // namespace protoscope::ui
