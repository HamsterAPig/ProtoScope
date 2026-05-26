#include "protoscope/config/config.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
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

const std::vector<std::string> kDefaultSerialPorts = {"COM1", "COM2", "COM3", "COM4"};

const char* kDefaultProtocolMainLua = R"PROTO(-- 核心流程：协议脚本只描述控件、收发和超时语义，底层 I/O 与 UI 统一由宿主的 proto.* API 承担。

function ui()
  return {
    {
      id = "protocol",
      title = "协议动作",
      controls = {
        { type = "button", id = "read_version", label = "读取版本" },
        { type = "input_text", id = "device_id", label = "设备 ID", default = "01" },
      }
    },
    {
      id = "advanced",
      title = "高级参数",
      controls = {
        { type = "checkbox", id = "hex_send", label = "HEX 发送", default = true },
        { type = "combo", id = "mode", label = "模式", options = { "轮询", "单次" }, default = 1 },
        { type = "input_int", id = "timeout_ms", label = "超时(ms)", default = 1000 },
        { type = "input_float", id = "scale", label = "缩放", default = 1.0 }
      }
    }
  }
end

local rx_buffer = {}

local function clear_rx_buffer()
  rx_buffer = {}
end

local function append_bytes(bytes)
  for i = 1, #bytes do
    rx_buffer[#rx_buffer + 1] = bytes[i]
  end
end

local function timeout_ms()
  return proto.get_control("timeout_ms") or 1000
end

local function device_id_byte()
  local device_id = tostring(proto.get_control("device_id") or "01")
  return string.byte(device_id, 1, 1) or 0x30
end

local function build_read_version_frame()
  if proto.get_control("hex_send") then
    return { 0xAA, 0x55, device_id_byte(), 0x01, 0x0D }
  end
  return "52 45 41 44 20 " .. string.format("%02X", device_id_byte())
end

local function parse_frame(bytes)
  if #bytes >= 2 and bytes[1] == string.byte("O") and bytes[2] == string.byte("K") then
    return {
      status = "ok",
      size = #bytes,
      mode = proto.get_control("mode"),
      scale = proto.get_control("scale")
    }
  end
  return nil
end

local function send_read_version(ctx)
  clear_rx_buffer()
  proto.send(build_read_version_frame())
  proto.set_timer("read_version_timeout", timeout_ms())
  proto.emit("request", { action = "read_version", connection_id = ctx.connection_id })
end

function on_open(ctx)
  proto.log("info", "连接已打开: " .. ctx.kind .. " -> " .. ctx.endpoint)
end

function on_close(ctx)
  proto.log("info", "连接已关闭: " .. ctx.endpoint)
end

function on_error(ctx, message)
  proto.log("error", "连接错误: " .. message)
end

function on_control(ctx, id, value)
  if id == "read_version" then
    send_read_version(ctx)
  else
    proto.log("info", "控件更新: " .. id .. "=" .. tostring(value))
  end
end

function on_bytes(ctx, bytes)
  append_bytes(bytes)
  local result = parse_frame(rx_buffer)
  if result then
    clear_rx_buffer()
    proto.cancel_timer("read_version_timeout")
    proto.emit("frame", result)
  else
    proto.emit("rx_bytes", { size = #bytes })
  end
end

function on_timer(ctx, name)
  if name == "read_version_timeout" then
    clear_rx_buffer()
    proto.emit("warning", { message = "读取版本超时", connection_id = ctx.connection_id })
  end
end
)PROTO";

const char* kDefaultGuide = R"(# ProtoScope Lua Host Guide

## 生命周期
- `on_open(ctx)`：连接打开后触发
- `on_close(ctx)`：连接关闭后触发
- `on_error(ctx, message)`：通讯层错误
- `on_bytes(ctx, bytes)`：收到原始字节
- `on_timer(ctx, name)`：定时器触发
- `on_control(ctx, id, value)`：宿主控件变化或按钮触发

## UI 描述
优先使用 `ui()` 返回多个 Dock：

```lua
function ui()
  return {
    {
      id = "protocol",
      title = "协议动作",
      controls = {
        { type = "button", id = "read_version", label = "读取版本" },
        { type = "input_text", id = "device_id", label = "设备 ID", default = "01" },
      }
    }
  }
end
```

兼容旧脚本时也可保留 `controls()`。

## proto API
- `proto.log(level, message)`
- `proto.send(hexStringOrByteArray)`
- `proto.emit(name, payload)`
- `proto.set_timer(name, delayMs)`
- `proto.cancel_timer(name)`
- `proto.get_control(id)`
- `proto.set_control(id, value)`
- `proto.crc16_modbus(payload)`
- `proto.crc16_ccitt_false(payload)`
- `proto.crc32_ieee(payload)`
)";

} // namespace

ConfigStore::ConfigStore()
    : defaultConfigPath_("config/protoscope.yaml"),
      defaultProtocolDir_("protocols/default_protocol") {}

AppConfig ConfigStore::withDefaults() const {
    AppConfig config;
    config.protocol.rootDir = normalizeTextPath(defaultProtocolDir_.parent_path());
    config.protocol.selectedDir = normalizeTextPath(defaultProtocolDir_);
    config.configPath = normalizeTextPath(defaultConfigPath_);
    config.communication.serialPortOptions = kDefaultSerialPorts;
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

    try {
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
        if (const auto configHotReload = app["config_hot_reload"]) {
            result.config.app.configHotReload.enabled =
                readScalar<bool>(configHotReload, "enabled", result.config.app.configHotReload.enabled);
        }

        const auto gui = root["gui"];
        if (const auto window = gui["window"]) {
            result.config.gui.window.title = readScalar<std::string>(window, "title", result.config.gui.window.title);
            result.config.gui.window.width = readScalar<int>(window, "width", result.config.gui.window.width);
            result.config.gui.window.height = readScalar<int>(window, "height", result.config.gui.window.height);
            result.config.gui.window.maximized = readScalar<bool>(window, "maximized", result.config.gui.window.maximized);
        }
        if (const auto wave = gui["wave"]) {
            result.config.gui.wave.maxRenderPointsPerChannel =
                readScalar<std::size_t>(wave, "max_render_points_per_channel", result.config.gui.wave.maxRenderPointsPerChannel);
            result.config.gui.wave.maxRenderVertices =
                readScalar<std::size_t>(wave, "max_render_vertices", result.config.gui.wave.maxRenderVertices);
            result.config.gui.wave.overviewMaxSamples =
                readScalar<std::size_t>(wave, "overview_max_samples", result.config.gui.wave.overviewMaxSamples);
        }

        const auto protocol = root["protocol"];
        result.config.protocol.rootDir = readScalar<std::string>(protocol, "root_dir", result.config.protocol.rootDir);
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
            result.config.communication.serial.dataBits =
                readScalar<std::uint32_t>(serial, "data_bits", result.config.communication.serial.dataBits);
            result.config.communication.serial.parity =
                readScalar<std::string>(serial, "parity", result.config.communication.serial.parity);
            result.config.communication.serial.stopBits =
                readScalar<std::string>(serial, "stop_bits", result.config.communication.serial.stopBits);
            result.config.communication.serial.flowControl =
                readScalar<std::string>(serial, "flow_control", result.config.communication.serial.flowControl);
        }
        if (const auto receive = root["receive"]) {
            result.config.communication.serialPortOptions = kDefaultSerialPorts;
            result.config.communication.reconnectRequired = false;
            result.config.communication.lastError.clear();
            result.config.communication.txCount = 0;
            result.config.communication.rxCount = 0;
            (void)receive;
        }
    } catch (const std::exception& ex) {
        result.error = std::string("读取 YAML 失败: ") + ex.what();
        result.loadedFromDisk = false;
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
    root["app"]["config_hot_reload"]["enabled"] = config.app.configHotReload.enabled;

    root["gui"]["window"]["title"] = config.gui.window.title;
    root["gui"]["window"]["width"] = config.gui.window.width;
    root["gui"]["window"]["height"] = config.gui.window.height;
    root["gui"]["window"]["maximized"] = config.gui.window.maximized;
    root["gui"]["wave"]["max_render_points_per_channel"] = config.gui.wave.maxRenderPointsPerChannel;
    root["gui"]["wave"]["max_render_vertices"] = config.gui.wave.maxRenderVertices;
    root["gui"]["wave"]["overview_max_samples"] = config.gui.wave.overviewMaxSamples;

    root["protocol"]["root_dir"] = config.protocol.rootDir;
    root["protocol"]["selected_dir"] = config.protocol.selectedDir;

    root["communication"]["kind"] = toTransportKindText(config.communication.kind);
    root["communication"]["tcp_client"]["host"] = config.communication.tcpClient.host;
    root["communication"]["tcp_client"]["port"] = config.communication.tcpClient.port;
    root["communication"]["tcp_server"]["bind_address"] = config.communication.tcpServer.bindAddress;
    root["communication"]["tcp_server"]["port"] = config.communication.tcpServer.port;
    root["communication"]["tcp_server"]["reject_new_connection"] = config.communication.tcpServer.rejectNewConnection;
    root["communication"]["serial"]["port_name"] = config.communication.serial.portName;
    root["communication"]["serial"]["baud_rate"] = config.communication.serial.baudRate;
    root["communication"]["serial"]["data_bits"] = config.communication.serial.dataBits;
    root["communication"]["serial"]["parity"] = config.communication.serial.parity;
    root["communication"]["serial"]["stop_bits"] = config.communication.serial.stopBits;
    root["communication"]["serial"]["flow_control"] = config.communication.serial.flowControl;

    try {
        if (!path.parent_path().empty()) {
            std::filesystem::create_directories(path.parent_path());
        }
        std::ofstream out(path);
        if (!out.good()) {
            error = "无法写入配置文件";
            return false;
        }
        out << root;
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

std::filesystem::path ConfigStore::normalizeProtocolDir(const std::filesystem::path& dir) const {
    return normalizeProtocolDir(defaultProtocolDir_.parent_path(), dir);
}

std::filesystem::path ConfigStore::normalizeProtocolDir(const std::filesystem::path& rootDir, const std::filesystem::path& dir) const {
    std::filesystem::path candidate = dir.empty() ? defaultProtocolDir_ : dir;
    if (protocolEntryExists(candidate)) {
        return candidate;
    }

    const auto scanned = scanProtocolDirectories(rootDir);
    if (!scanned.empty()) {
        return std::filesystem::path(scanned.front());
    }
    return defaultProtocolDir_;
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

std::vector<std::string> ConfigStore::scanProtocolDirectories(const std::filesystem::path& rootDir) const {
    std::vector<std::string> results;
    std::error_code ec;
    if (!std::filesystem::exists(rootDir, ec) || ec) {
        return results;
    }

    for (const auto& entry : std::filesystem::directory_iterator(rootDir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_directory()) {
            continue;
        }
        if (!protocolEntryExists(entry.path())) {
            continue;
        }
        results.push_back(normalizeTextPath(entry.path()));
    }

    std::sort(results.begin(), results.end());
    return results;
}

std::filesystem::path ConfigStore::defaultScriptWorkspaceDir() const {
    return "scripts";
}

std::filesystem::path ConfigStore::defaultScriptHelpPath() const {
    return defaultScriptWorkspaceDir() / "README.txt";
}

bool ConfigStore::ensureDefaultProtocolScript(const std::filesystem::path& protocolDir, std::string& error) const {
    try {
        std::filesystem::create_directories(protocolDir);
        const auto scriptPath = mainLuaPath(protocolDir);
        if (!std::filesystem::exists(scriptPath)) {
            std::ofstream out(scriptPath);
            if (!out.good()) {
                error = "无法写入默认协议脚本";
                return false;
            }
            out << kDefaultProtocolMainLua;
        }
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

bool ConfigStore::ensureDefaultScriptWorkspace(std::string& error) const {
    try {
        if (!ensureDefaultProtocolScript(defaultProtocolDir_, error)) {
            return false;
        }
        std::filesystem::create_directories(defaultScriptWorkspaceDir());
        if (!std::filesystem::exists(defaultScriptHelpPath())) {
            std::ofstream out(defaultScriptHelpPath());
            out << kDefaultGuide;
        }
        const auto sampleScript = defaultScriptWorkspaceDir() / "main.lua";
        if (!std::filesystem::exists(sampleScript) && std::filesystem::exists(mainLuaPath(defaultProtocolDir_))) {
            std::filesystem::copy_file(mainLuaPath(defaultProtocolDir_), sampleScript, std::filesystem::copy_options::skip_existing);
        }
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
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
        comm.serialPortOptions = kDefaultSerialPorts;
    }

    const auto protocolDir = normalizeProtocolDir(config.protocol.rootDir, config.protocol.selectedDir);
    auto& lua = dockStore.luaState();
    lua.protocolRootDir = config.protocol.rootDir;
    lua.protocolDirOptions = scanProtocolDirectories(lua.protocolRootDir);
    lua.protocolDir = normalizeTextPath(protocolDir);
    lua.protocolName = protocolName(protocolDir);
    lua.scriptPath = normalizeTextPath(mainLuaPath(protocolDir));

    auto& configState = dockStore.configState();
    configState.autoSaveEnabled = config.app.autoSave.enabled;
    configState.autoSaveIntervalMs = config.app.autoSave.intervalMs;
    configState.configHotReloadEnabled = config.app.configHotReload.enabled;
    configState.fpsLimit = config.app.fpsLimit;
    configState.idleRender = config.app.idleRender;
    configState.loadedFromPath = config.configPath.empty() ? normalizeTextPath(defaultConfigPath_) : config.configPath;

    auto& wave = dockStore.waveState().view;
    wave.maxRenderPointsPerChannel = config.gui.wave.maxRenderPointsPerChannel;
    wave.maxRenderVertices = config.gui.wave.maxRenderVertices;
    wave.overviewMaxSamples = config.gui.wave.overviewMaxSamples;
}

AppConfig ConfigStore::captureFromDock(const dock::DockStore& dockStore) const {
    AppConfig config = withDefaults();

    config.communication = dockStore.commState();
    config.protocol.rootDir = dockStore.luaState().protocolRootDir;
    config.protocol.selectedDir = dockStore.luaState().protocolDir;
    config.app.autoSave.enabled = dockStore.configState().autoSaveEnabled;
    config.app.autoSave.intervalMs = dockStore.configState().autoSaveIntervalMs;
    config.app.configHotReload.enabled = dockStore.configState().configHotReloadEnabled;
    config.app.fpsLimit = dockStore.configState().fpsLimit;
    config.app.idleRender = dockStore.configState().idleRender;
    config.gui.wave.maxRenderPointsPerChannel = dockStore.waveState().view.maxRenderPointsPerChannel;
    config.gui.wave.maxRenderVertices = dockStore.waveState().view.maxRenderVertices;
    config.gui.wave.overviewMaxSamples = dockStore.waveState().view.overviewMaxSamples;
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
