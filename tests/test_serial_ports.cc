#include "test_registry.hpp"

#include "protoscope/transport/transport.hpp"

#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

void test_serial_port_name_normalization() {
    using namespace protoscope::transport;

    const auto ports = normalizeSerialPortNames({"COM10", "", "COM2", "COM1", "COM2", "/dev/ttyUSB1"});

    require(ports.size() == 4, "串口列表应过滤空值并去重");
    require(ports[0] == "COM1", "COM1 应排在最前");
    require(ports[1] == "COM2", "COM2 应按数字顺序排在 COM10 前");
    require(ports[2] == "COM10", "COM10 应按数字顺序排序");
    require(ports[3] == "/dev/ttyUSB1", "非 COM 端口应保留在结果中");
}
