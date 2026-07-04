#include "protoscope/config/config.hpp"
#include "protoscope/scripting/script_host.hpp"
#include "protoscope/transport/transport.hpp"

#include "test_helpers.hpp"
#include "test_registry.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

void test_headless_script_host_loads_default_protocol()
{
    protoscope::scripting::ScriptHost host;
    protoscope::tests::require(host.loadProtocolDirectory("protocols/default_protocol"),
                               "headless 默认协议目录应可加载");
    protoscope::tests::require(host.protocolDirectory() == "protocols/default_protocol",
                               "headless 协议目录应被记录");
    protoscope::tests::require(!host.controlsSnapshot().empty(), "headless 默认协议应声明可见控件");
}

void test_headless_config_default_roundtrip()
{
    protoscope::config::ConfigStore store;
    const protoscope::tests::ScopedTempPath tempRoot(
        protoscope::tests::makeUniqueTempDir("protoscope-headless-config-roundtrip"));
    const auto tempPath = tempRoot.path() / "protoscope.yaml";

    auto config = store.load(tempPath).config;
    protoscope::tests::require(config.protocol.rootDir.find("protocols/templates") != std::string::npos,
                               "headless 默认协议根目录应指向 protocols/templates");
    config.communication.kind = protoscope::transport::TransportKind::UdpPeer;
    config.communication.udpPeer.bindAddress = "127.0.0.1";
    config.communication.udpPeer.bindPort = 19001;
    config.communication.udpPeer.remoteHost = "192.0.2.10";
    config.communication.udpPeer.remotePort = 19002;
    config.scripting.workerBatchBytes = 2048U;

    std::string error;
    protoscope::tests::require(store.save(tempPath, config, error), "headless 默认配置写回失败");

    const auto reloaded = store.load(tempPath);
    if (!reloaded.error.empty()) {
        throw std::runtime_error("headless 配置读回失败: " + reloaded.error);
    }
    protoscope::tests::require(reloaded.config.communication.kind == protoscope::transport::TransportKind::UdpPeer,
                               "headless UDP Peer 模式 roundtrip 失败");
    protoscope::tests::require(reloaded.config.communication.udpPeer.remoteHost == "192.0.2.10",
                               "headless UDP Peer 远端地址 roundtrip 失败");
    protoscope::tests::require(reloaded.config.scripting.workerBatchBytes == 2048U,
                               "headless scripting worker batch roundtrip 失败");
}
