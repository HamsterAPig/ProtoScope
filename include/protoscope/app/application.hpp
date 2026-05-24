#pragma once

#include "protoscope/config/config.hpp"
#include "protoscope/dock/docks.hpp"
#include "protoscope/scripting/script_host.hpp"
#include "protoscope/transport/transport.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace protoscope::app {

class Application {
public:
    Application();

    bool initialize();
    bool applyConfig(const config::AppConfig& config);
    config::AppConfig captureConfig() const;
    bool reloadProtocolDirectory(const std::string& protocolDir, bool forceReload = false);
    bool pumpOnce();
    void shutdown();

    dock::DockStore& docks();
    const dock::DockStore& docks() const;

    void openTransport();
    void closeTransport();
    bool sendManualPayload(const std::string& payload, bool hexMode);
    void triggerAction(const std::string& actionName);
    void updateControlValue(const std::string& id, const scripting::ControlValue& value);
    void markCommConfigEdited(bool reconnectRequired);
    void markProtocolEdited();

    std::optional<std::uint64_t> nextWakeupAtMs() const;

private:
    std::unique_ptr<transport::ITransport> createTransport(transport::TransportKind kind) const;
    void syncDockState();
    bool handleTransportEvents();
    bool flushScriptOutputs();
    bool flushScriptLogs();

private:
    dock::DockStore dockStore_;
    config::ConfigStore configStore_{};
    config::AppConfig runtimeConfig_{};
    scripting::ScriptHost scriptHost_;
    std::unique_ptr<transport::ITransport> transport_;
    std::optional<transport::ConnectionContext> activeConnection_;
};

} // namespace protoscope::app
