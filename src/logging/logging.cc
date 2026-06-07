#include "protoscope/logging/logging.hpp"

#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace protoscope::logging {

namespace {

    spdlog::level::level_enum toSpdlogLevel(const config::LogLevel level)
    {
        switch (level) {
            case config::LogLevel::Debug:
                return spdlog::level::debug;
            case config::LogLevel::Info:
                return spdlog::level::info;
            case config::LogLevel::Warn:
                return spdlog::level::warn;
            case config::LogLevel::Error:
                return spdlog::level::err;
        }
        return spdlog::level::info;
    }

} // namespace

struct LoggingFacade::Impl {
    std::shared_ptr<spdlog::logger> logger;
    std::shared_ptr<spdlog::sinks::stdout_color_sink_mt> consoleSink;
    std::shared_ptr<spdlog::sinks::basic_file_sink_mt> fileSink;
    std::string activeFilePath;
};

LoggingFacade::LoggingFacade() : impl_(std::make_unique<Impl>())
{
    rebuildLogger();
}

LoggingFacade::~LoggingFacade() = default;

void LoggingFacade::bindDockStore(dock::DockStore* dockStore)
{
    dockStore_ = dockStore;
}

void LoggingFacade::applyConfig(const config::AppLoggingConfig& config)
{
    config_ = config;
    rebuildLogger();
}

void LoggingFacade::debug(std::string endpoint, std::string message)
{
    log(LogRecord{
        .level = config::LogLevel::Debug,
        .audience = LogAudience::Host,
        .direction = levelDirection(config::LogLevel::Debug),
        .endpoint = std::move(endpoint),
        .message = std::move(message),
        .timestampMs = nowMs(),
    });
}

void LoggingFacade::info(std::string endpoint, std::string message)
{
    log(LogRecord{
        .level = config::LogLevel::Info,
        .audience = LogAudience::Host,
        .direction = levelDirection(config::LogLevel::Info),
        .endpoint = std::move(endpoint),
        .message = std::move(message),
        .timestampMs = nowMs(),
    });
}

void LoggingFacade::warn(std::string endpoint, std::string message)
{
    log(LogRecord{
        .level = config::LogLevel::Warn,
        .audience = LogAudience::Host,
        .direction = levelDirection(config::LogLevel::Warn),
        .endpoint = std::move(endpoint),
        .message = std::move(message),
        .timestampMs = nowMs(),
    });
}

void LoggingFacade::error(std::string endpoint, std::string message)
{
    log(LogRecord{
        .level = config::LogLevel::Error,
        .audience = LogAudience::Host,
        .direction = levelDirection(config::LogLevel::Error),
        .endpoint = std::move(endpoint),
        .message = std::move(message),
        .timestampMs = nowMs(),
    });
}

void LoggingFacade::host(config::LogLevel level,
                         std::string direction,
                         std::string endpoint,
                         std::string message,
                         std::optional<std::uint64_t> timestampMs)
{
    log(LogRecord{
        .level = level,
        .audience = LogAudience::Host,
        .direction = std::move(direction),
        .endpoint = std::move(endpoint),
        .message = std::move(message),
        .timestampMs = timestampMs.value_or(nowMs()),
    });
}

void LoggingFacade::script(std::string levelTextValue, std::string message, std::optional<std::uint64_t> timestampMs)
{
    bool recognized = false;
    const auto level = parseLevelOrFallback(levelTextValue, config::LogLevel::Info, &recognized);
    if (!recognized) {
        warn("logging", "未知脚本日志等级 '" + levelTextValue + "'，已按 info 处理");
        levelTextValue = "info";
    }

    log(LogRecord{
        .level = level,
        .audience = LogAudience::Script,
        .direction = "LOG",
        .endpoint = "script",
        .message = "[" + levelTextValue + "] " + message,
        .timestampMs = timestampMs.value_or(nowMs()),
    });
}

config::AppLoggingConfig LoggingFacade::currentConfig() const
{
    return config_;
}

void LoggingFacade::log(LogRecord record)
{
    if (record.level < config_.level) {
        return;
    }

    if (impl_->logger) {
        impl_->logger->log(toSpdlogLevel(record.level), "[{}] {}", record.endpoint, record.message);
        impl_->logger->flush();
    }

    if (!dockStore_) {
        return;
    }

    dock::ReceiveRow row{
        .timestampMs = record.timestampMs,
        .direction = record.direction,
        .endpoint = record.endpoint,
        .bytes = {},
        .message = record.message,
    };

    if (record.audience == LogAudience::Script) {
        dockStore_->appendScriptRow(std::move(row));
    } else {
        dockStore_->appendLogRow(std::move(row));
    }
}

void LoggingFacade::rebuildLogger()
{
    impl_->consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    impl_->consoleSink->set_level(toSpdlogLevel(config_.level));
    impl_->fileSink.reset();
    impl_->activeFilePath.clear();
    std::string sinkError;

    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(impl_->consoleSink);

    std::string filePath = config_.filePath;
    if (!filePath.empty()) {
        try {
            std::filesystem::path resolved(filePath);
            if (!resolved.parent_path().empty()) {
                std::filesystem::create_directories(resolved.parent_path());
            }
            impl_->fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(resolved.string(), true);
            impl_->fileSink->set_level(toSpdlogLevel(config_.level));
            impl_->activeFilePath = resolved.generic_string();
            sinks.push_back(impl_->fileSink);
        } catch (const std::exception& ex) {
            config_.filePath.clear();
            sinkError = ex.what();
        }
    }

    impl_->logger = std::make_shared<spdlog::logger>("protoscope", sinks.begin(), sinks.end());
    impl_->logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%^%l%$] %v");
    impl_->logger->set_level(toSpdlogLevel(config_.level));
    impl_->logger->flush_on(toSpdlogLevel(config::LogLevel::Debug));
    spdlog::set_default_logger(impl_->logger);
    spdlog::set_level(toSpdlogLevel(config_.level));

    if (!sinkError.empty() && !reportingSinkFailure_) {
        reportingSinkFailure_ = true;
        error("logging", "日志文件不可用，已降级为控制台 + UI: " + sinkError);
        reportingSinkFailure_ = false;
    }
    if (!impl_->activeFilePath.empty()) {
        info("logging", "日志文件已切换到: " + impl_->activeFilePath);
    }
}

config::LogLevel LoggingFacade::parseLevelOrFallback(const std::string& levelTextValue,
                                                     const config::LogLevel fallback,
                                                     bool* recognized)
{
    if (recognized) {
        *recognized = true;
    }
    if (levelTextValue == "debug") {
        return config::LogLevel::Debug;
    }
    if (levelTextValue == "info") {
        return config::LogLevel::Info;
    }
    if (levelTextValue == "warn" || levelTextValue == "warning") {
        return config::LogLevel::Warn;
    }
    if (levelTextValue == "error") {
        return config::LogLevel::Error;
    }
    if (recognized) {
        *recognized = false;
    }
    return fallback;
}

std::string LoggingFacade::levelText(const config::LogLevel level)
{
    switch (level) {
        case config::LogLevel::Debug:
            return "debug";
        case config::LogLevel::Info:
            return "info";
        case config::LogLevel::Warn:
            return "warn";
        case config::LogLevel::Error:
            return "error";
    }
    return "info";
}

std::string LoggingFacade::levelDirection(const config::LogLevel level)
{
    switch (level) {
        case config::LogLevel::Debug:
            return "DEBUG";
        case config::LogLevel::Info:
            return "INFO";
        case config::LogLevel::Warn:
            return "WARN";
        case config::LogLevel::Error:
            return "ERROR";
    }
    return "INFO";
}

std::uint64_t LoggingFacade::nowMs()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

} // namespace protoscope::logging
