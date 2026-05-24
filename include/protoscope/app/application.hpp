#pragma once

#include "protoscope/dock/docks.hpp"
#include "protoscope/scripting/script_host.hpp"
#include "protoscope/transport/transport.hpp"

#include <memory>
#include <optional>
#include <string>

namespace protoscope::app {

class Application {
public:
    Application();

    bool initialize();
    void pumpOnce();
    void shutdown();

    dock::DockStore& docks();
    const dock::DockStore& docks() const;

    void openTransport();
    void closeTransport();
    bool sendManualPayload(const std::string& payload, bool hexMode);
    void triggerAction(const std::string& actionName);
    void updateControlValue(const std::string& id, const scripting::ControlValue& value);

private:
    std::unique_ptr<transport::ITransport> createTransport(transport::TransportKind kind) const;
    void syncDockState();
    void handleTransportEvents();
    void flushScriptOutputs();

private:
    dock::DockStore dockStore_;
    scripting::ScriptHost scriptHost_;
    std::unique_ptr<transport::ITransport> transport_;
    std::optional<transport::ConnectionContext> activeConnection_;
};

} // namespace protoscope::app
