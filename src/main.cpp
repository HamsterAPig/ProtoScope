#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <string>

int main() {
    // 核心流程：先验证日志库与配置解析库都已正确链接。
    const auto sample = YAML::Load(R"(
app:
  name: ProtoScope
  mode: bootstrap
)");

    // 核心流程：输出启动信息，后续可以在这里继续挂接 ELF/DWARF 与 UI 初始化。
    const std::string appName = sample["app"]["name"].as<std::string>();
    const std::string mode = sample["app"]["mode"].as<std::string>();

    spdlog::info("Starting {} in {} mode.", appName, mode);
    return 0;
}
