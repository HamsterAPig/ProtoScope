#include "protoscope/config/config.hpp"

#include <yaml-cpp/yaml.h>

#include <chrono>
#include <fstream>
#include <system_error>

namespace protoscope::config {

namespace {

template <typename T>
T readScalar(const YAML::Node& node, const char* key, T fallback) {
    if (!node || !node[key]) {
        return fallback;
    }
    return node[key].as<T>();
}

std::string normalizeTextPath(std::filesystem::path path) {
    path.make_preferred();
    return path.generic_string();
}

transport::TransportKind parseTransportKind(const std::string& value) {
    if (value == "tcp_server") {
        return transport::TransportKind::TcpServer;
    }
    if (value == "serial") {
        return transport::TransportKind::Serial;
    }
    return transport::TransportKind::TcpClient;
}

std::string toTransportKindText(transport::TransportKind kind) {
    switch (kind) {
    case transport::TransportKind::TcpClient:
        return "tcp_client";
    case transport::TransportKind::TcpServer:
        return "tcp_server";
    case transport::TransportKind::Serial:
        return "serial";
    }
    return "tcp_client";
}

} // namespace

ConfigStore::ConfigStore()
    : defaultConfigPath_("config/protoscope.yaml"),
      defaultProtocolDir_("protocols/default_protocol") {}

AppConfig ConfigStore::withDefaults() const {
    AppConfig config;
    config.protocol.selectedDir = normalizeTextPath(defaultProtocolDir_);
    config.configPath = normalizeTextPath(defaultConfigPath_);
    config.communication.serialPortOptions = {"COM1", "COM2", "COM3"};
    return config;
}

ConfigLoadResult ConfigStore::load(const std::filesystem::path& path) const {
    ConfigLoadResult result;
    result.config = withDefaults();
    result.resolvedPath = path.empty() ? defaultConfigPath_ : path;

    if (!std::filesystem::exists(result.resolvedPath)) {
        result.config.configPath = normalizeTextPath(result.resolvedPath);
        return result;
    }

    const YAML::Node root = YAML::LoadFile(result.resolvedPath.string());
    result.loadedFromDisk = true;

    const auto app = root["app"];
    result.config.app.language = readScalar<std::string>(app, "language", result.config.app.language);
    result.config.app.fpsLimit = readScalar<std::uint32_t>(app, "fps_limit", result.config.app.fpsLimit);
    result.config.app.idleRender = readScalar<std::string>(app, "idle_render", result.config.app.idleRender);
    if (const auto autoSave = app["auto_save"]) {
        result.config.app.autoSave.enabled = readScalar<bool>(autoSave, "enabled", result.config.app.autoSave.enabled);
        result.config.app.autoSave.intervalMs = readScalar<std::uint64_t>(autoSave, "interval_ms", result.config.app.autoSave.intervalMs);
    }

    const auto gui = root["gui"];
    if (const auto window = gui["window"]) {
        result.config.gui.window.title = readScalar<std::string>(window, "title", result.config.gui.window.title);
        result.config.gui.window.width = readScalar<int>(window, "width", result.config.gui.window.width);
        result.config.gui.window.height = readScalar<int>(window, "height", result.config.gui.window.height);
        result.config.gui.window.maximized = readScalar<bool>(window, "maximized", result.config.gui.window.maximized);
    }

    const auto protocol = root["protocol"];
    result.config.protocol.selectedDir = readScalar<std::string>(protocol, "selected_dir", result.config.protocol.selectedDir);
    result.config.configPath = normalizeTextPath(result.resolvedPath);

    const auto communication = root["communication"];
    result.config.communication.kind =
        parseTransportKind(readScalar<std::string>(communication, "kind", toTransportKindText(result.config.communication.kind)));

    if (const auto tcpClient = communication["tcp_client"]) {
        result.config.communication.tcpClient.host =
            readScalar<std::string>(tcpClient, "host", result.config.communication.tcpClient.host);
        result.config.communication.tcpClient.port =
            readScalar<std::uint16_t>(tcpClient, "port", result.config.communication.tcpClient.port);
    }

    if (const auto tcpServer = communication["tcp_server"]) {
        result.config.communication.tcpServer.bindAddress =
            readScalar<std::string>(tcpServer, "bind_address", result.config.communication.tcpServer.bindAddress);
        result.config.communication.tcpServer.port =
            readScalar<std::uint16_t>(tcpServer, "port", result.config.communication.tcpServer.port);
        result.config.communication.tcpServer.rejectNewConnection =
            readScalar<bool>(tcpServer, "reject_new_connection", result.config.communication.tcpServer.rejectNewConnection);
    }

    if (const auto serial = communication["serial"]) {
        result.config.communication.serial.portName =
            readScalar<std::string>(serial, "port_name", result.config.communication.serial.portName);
        result.config.communication.serial.baudRate =
            readScalar<std::uint32_t>(serial, "baud_rate", result.config.communication.serial.baudRate);
    }

    return result;
}

bool ConfigStore::save(const std::filesystem::path& path, const AppConfig& config, std::string& error) const {
    YAML::Node root;

    root["app"]["language"] = config.app.language;
    root["app"]["fps_limit"] = config.app.fpsLimit;
    root["app"]["idle_render"] = config.app.idleRender;
    root["app"]["auto_save"]["enabled"] = config.app.autoSave.enabled;
    root["app"]["auto_save"]["interval_ms"] = config.app.autoSave.intervalMs;

    root["gui"]["window"]["title"] = config.gui.window.title;
    root["gui"]["window"]["width"] = config.gui.window.width;
    root["gui"]["window"]["height"] = config.gui.window.height;
    root["gui"]["window"]["maximized"] = config.gui.window.maximized;

    root["protocol"]["selected_dir"] = config.protocol.selectedDir;

    root["communication"]["kind"] = toTransportKindText(config.communication.kind);
    root["communication"]["tcp_client"]["host"] = config.communication.tcpClient.host;
    root["communication"]["tcp_client"]["port"] = config.communication.tcpClient.port;
    root["communication"]["tcp_server"]["bind_address"] = config.communication.tcpServer.bindAddress;
    root["communication"]["tcp_server"]["port"] = config.communication.tcpServer.port;
    root["communication"]["tcp_server"]["reject_new_connection"] = config.communication.tcpServer.rejectNewConnection;
    root["communication"]["serial"]["port_name"] = config.communication.serial.portName;
    root["communication"]["serial"]["baud_rate"] = config.communication.serial.baudRate;

    try {
        if (!path.parent_path().empty()) {
            std::filesystem::create_directories(path.parent_path());
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << root;
        out.flush();
        if (!out.good()) {
            error = "写入 YAML 文件失败";
            return false;
        }
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }

    return true;
}

std::filesystem::path ConfigStore::normalizeProtocolDir(const std::filesystem::path& dir) const {
    std::filesystem::path candidate = dir.empty() ? defaultProtocolDir_ : dir;
    if (!std::filesystem::exists(candidate) || !protocolEntryExists(candidate)) {
        candidate = defaultProtocolDir_;
    }
    return candidate;
}

std::filesystem::path ConfigStore::mainLuaPath(const std::filesystem::path& protocolDir) const {
    return protocolDir / "main.lua";
}

std::string ConfigStore::protocolName(const std::filesystem::path& protocolDir) const {
    const auto filename = protocolDir.filename().string();
    return filename.empty() ? std::string("default_protocol") : filename;
}

bool ConfigStore::protocolEntryExists(const std::filesystem::path& protocolDir) const {
    return std::filesystem::exists(mainLuaPath(protocolDir));
}

FileSnapshot ConfigStore::snapshot(const std::filesystem::path& path) const {
    FileSnapshot result;
    result.path = path;
    result.exists = std::filesystem::exists(path);
    if (result.exists) {
        result.timestampMs = toTimestampMs(std::filesystem::last_write_time(path));
    }
    return result;
}

bool ConfigStore::hasChanged(const FileSnapshot& previous) const {
    const auto current = snapshot(previous.path);
    return current.exists != previous.exists || current.timestampMs != previous.timestampMs;
}

void ConfigStore::applyToDock(const AppConfig& config, dock::DockStore& dockStore) const {
    auto& comm = dockStore.commState();
    comm.kind = config.communication.kind;
    comm.tcpClient = config.communication.tcpClient;
    comm.tcpServer = config.communication.tcpServer;
    comm.serial = config.communication.serial;
    if (comm.serialPortOptions.empty()) {
        comm.serialPortOptions = {"COM1", "COM2", "COM3"};
    }

    const auto protocolDir = normalizeProtocolDir(config.protocol.selectedDir);
    auto& lua = dockStore.luaState();
    lua.protocolDir = normalizeTextPath(protocolDir);
    lua.protocolName = protocolName(protocolDir);
    lua.scriptPath = normalizeTextPath(mainLuaPath(protocolDir));

    auto& configState = dockStore.configState();
    configState.autoSaveEnabled = config.app.autoSave.enabled;
    configState.autoSaveIntervalMs = config.app.autoSave.intervalMs;
    configState.fpsLimit = config.app.fpsLimit;
    configState.idleRender = config.app.idleRender;
    configState.loadedFromPath = config.configPath.empty() ? normalizeTextPath(defaultConfigPath_) : config.configPath;
}

AppConfig ConfigStore::captureFromDock(const dock::DockStore& dockStore) const {
    AppConfig config = withDefaults();

    config.communication = dockStore.commState();
    config.protocol.selectedDir = dockStore.luaState().protocolDir;
    config.app.autoSave.enabled = dockStore.configState().autoSaveEnabled;
    config.app.autoSave.intervalMs = dockStore.configState().autoSaveIntervalMs;
    config.app.fpsLimit = dockStore.configState().fpsLimit;
    config.app.idleRender = dockStore.configState().idleRender;
    config.configPath = dockStore.configState().loadedFromPath;

    return config;
}

const std::filesystem::path& ConfigStore::defaultConfigPath() const {
    return defaultConfigPath_;
}

const std::filesystem::path& ConfigStore::defaultProtocolDir() const {
    return defaultProtocolDir_;
}

std::uint64_t ConfigStore::toTimestampMs(const std::filesystem::file_time_type& fileTime) {
    const auto normalized = fileTime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(normalized.time_since_epoch()).count());
}

} // namespace protoscope::config
