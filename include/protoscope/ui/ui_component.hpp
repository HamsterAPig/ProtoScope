#pragma once

#include "protoscope/ui/ui_host_context.hpp"

#include <memory>
#include <string_view>
#include <vector>

namespace protoscope::ui {

class IUiComponent {
public:
    virtual ~IUiComponent() = default;

    virtual std::string_view id() const = 0;

    virtual void onAttach(RuntimeUiContext&) {}

    virtual void onDetach(RuntimeUiContext&) {}

    virtual void onFrame(RuntimeUiContext&) {}
};

class IMenuContributor : public IUiComponent {
public:
    virtual void drawMainMenuItems(RuntimeUiContext&) = 0;
};

class IDialogComponent : public IUiComponent {
public:
    virtual void syncRequests(RuntimeUiContext&) {}

    virtual void drawDialogs(RuntimeUiContext&) = 0;
};

class IDockComponent : public IUiComponent {
public:
    virtual std::string_view title() const = 0;
    virtual bool defaultVisible() const = 0;
    virtual void drawDock(RuntimeUiContext&) = 0;
};

class UiComponentRegistry {
public:
    using ComponentList = std::vector<std::unique_ptr<IUiComponent>>;

    static ComponentList createRuntimeComponents(class GuiRuntime& runtime);
};

} // namespace protoscope::ui
