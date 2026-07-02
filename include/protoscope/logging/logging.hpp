#pragma once

#include "protoscope/config/config.hpp"
#include "protoscope/dock/docks.hpp"

#include <memory>
#include <optional>
#include <string>

namespace protoscope::logging {

enum class LogAudience {
    Host,
    Script,
};

struct LogRecord {
    config::LogLevel level{config::LogLevel::Info};
    LogAudience audience{LogAudience::Host};
    std::string direction;
    std::string endpoint;
    std::string message;
    std::uint64_t timestampMs{0};
};

class LoggingFacade {
public:
    LoggingFacade();
    ~LoggingFacade();

    void bindDockStore(dock::DockStore* dockStore);
    void applyConfig(const config::AppLoggingConfig& config);

    void trace(std::string endpoint, std::string message);
    void debug(std::string endpoint, std::string message);
    void info(std::string endpoint, std::string message);
    void warn(std::string endpoint, std::string message);
    void error(std::string endpoint, std::string message);
    void host(config::LogLevel level,
              std::string direction,
              std::string endpoint,
              std::string message,
              std::optional<std::uint64_t> timestampMs = std::nullopt);
    void script(std::string levelText, std::string message, std::optional<std::uint64_t> timestampMs = std::nullopt);

    config::AppLoggingConfig currentConfig() const;

private:
    void log(LogRecord record);
    void rebuildLogger();
    static config::LogLevel parseLevelOrFallback(const std::string& levelText,
                                                 config::LogLevel fallback,
                                                 bool* recognized = nullptr);
    static std::string levelText(config::LogLevel level);
    static std::string levelDirection(config::LogLevel level);
    static std::uint64_t nowMs();

private:
    struct Impl;

    dock::DockStore* dockStore_{nullptr};
    config::AppLoggingConfig config_{};
    std::unique_ptr<Impl> impl_;
    bool reportingSinkFailure_{false};
};

} // namespace protoscope::logging
