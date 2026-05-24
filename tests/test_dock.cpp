#include "test_registry.hpp"

#include "protoscope/dock/docks.hpp"

#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

void test_config_external_reload_state() {
    protoscope::dock::DockStore store;
    auto& config = store.configState();

    require(!config.pendingExternalReload, "初始状态不应挂起外部重载");
    require(config.pendingExternalReloadTimestampMs == 0, "初始挂起时间戳应为 0");
    require(config.externalReloadMessage.empty(), "初始外部重载提示应为空");

    store.setPendingExternalReload(1234, "检测到外部配置更新");
    require(config.pendingExternalReload, "设置后应进入外部重载挂起态");
    require(config.pendingExternalReloadTimestampMs == 1234, "挂起时间戳应与设置值一致");
    require(config.externalReloadMessage == "检测到外部配置更新", "挂起提示应被记录");

    store.clearPendingExternalReload();
    require(!config.pendingExternalReload, "清理后不应继续挂起外部重载");
    require(config.pendingExternalReloadTimestampMs == 0, "清理后挂起时间戳应重置");
    require(config.externalReloadMessage.empty(), "清理后外部重载提示应清空");
}
