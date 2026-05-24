#include "protoscope/app/application.hpp"
#include "protoscope/transport/transport.hpp"

#include <spdlog/spdlog.h>

int main() {
    // 核心流程：第一版先打通 TCP+Lua+Dock 的业务总线，GUI 渲染后续再接入 GLFW+ImGui backend。
    protoscope::app::Application app;
    if (!app.initialize()) {
        spdlog::error("ProtoScope 初始化失败");
        return 1;
    }

    app.docks().commState().kind = protoscope::transport::TransportKind::TcpClient;
    app.openTransport();
    app.updateControlValue("device_id", std::string("01"));
    app.triggerAction("read_version");
    app.pumpOnce();
    app.shutdown();

    const auto& receiveRows = app.docks().receiveState().rows;
    spdlog::info("ProtoScope v1 pipeline finished. receive_rows={}", receiveRows.size());
    return 0;
}
