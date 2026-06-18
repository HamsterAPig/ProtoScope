#include "protoscope/ui/protocol_state_file.hpp"
#include "protoscope/ui/protocol_ui_state.hpp"

#include "test_helpers.hpp"
#include "test_registry.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace {

using protoscope::tests::makeUniqueTempDir;
using protoscope::tests::require;
using protoscope::tests::ScopedTempPath;

std::filesystem::path makeTempRoot(std::string_view name)
{
    return makeUniqueTempDir("protoscope-" + std::string(name));
}

void writeText(const std::filesystem::path& path, std::string_view text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << text;
}

} // namespace

void test_protocol_state_file_backs_up_corrupt_yaml()
{
    const ScopedTempPath root(makeTempRoot("protocol-state-corrupt"));
    const auto statePath = root.path() / "config" / "ui" / "protocol-control-state.yaml";
    writeText(statePath, "protocols:\n  proto_a:\n    send:\n      history: []es: ~\n");

    const auto loadResult = protoscope::ui::loadProtocolStateRootForUpdate(statePath);

    require(loadResult.ok, "损坏 YAML 应能备份后继续使用空状态");
    require(loadResult.recovery.recoveredCorruptFile, "损坏 YAML 应标记已恢复");
    require(std::filesystem::exists(loadResult.recovery.backupPath), "损坏 YAML 应被备份");
    require(!std::filesystem::exists(statePath), "损坏原文件应被移走，避免下次继续解析失败");
    require(loadResult.root.IsMap(), "恢复后的 root 应是空 map");
}

void test_protocol_state_file_atomic_write_replaces_valid_yaml()
{
    const ScopedTempPath root(makeTempRoot("protocol-state-atomic"));
    const auto statePath = root.path() / "config" / "ui" / "protocol-control-state.yaml";

    YAML::Node stateRoot;
    stateRoot["protocols"]["proto_a"]["controls"]["device_id"]["type"] = "input_text";
    stateRoot["protocols"]["proto_a"]["controls"]["device_id"]["value"] = "01";

    std::string error;
    require(protoscope::ui::writeProtocolStateRootAtomically(statePath, stateRoot, error), "原子写入应创建状态文件");

    const auto parsed = YAML::LoadFile(statePath.string());
    require(parsed["protocols"]["proto_a"]["controls"]["device_id"]["value"].as<std::string>() == "01",
            "写入后的 YAML 应可解析并保留值");
}

void test_protocol_state_file_preserves_other_protocol_nodes()
{
    const ScopedTempPath root(makeTempRoot("protocol-state-merge"));
    const auto statePath = root.path() / "config" / "ui" / "protocol-control-state.yaml";

    YAML::Node initialRoot;
    initialRoot["protocols"]["proto_a"]["controls"]["device_id"]["type"] = "input_text";
    initialRoot["protocols"]["proto_a"]["controls"]["device_id"]["value"] = "01";

    std::string error;
    require(protoscope::ui::writeProtocolStateRootAtomically(statePath, initialRoot, error), "初始状态文件应写入成功");

    auto loadResult = protoscope::ui::loadProtocolStateRootForUpdate(statePath);
    require(loadResult.ok, "合法 YAML 应能读取");
    loadResult.root["protocols"]["proto_b"]["controls"]["mode"]["type"] = "combo";
    loadResult.root["protocols"]["proto_b"]["controls"]["mode"]["value"] = 1;
    require(protoscope::ui::writeProtocolStateRootAtomically(statePath, loadResult.root, error),
            "合并后的状态文件应写入成功");

    const auto parsed = YAML::LoadFile(statePath.string());
    require(parsed["protocols"]["proto_a"].IsDefined(), "合并写入不应删除其他协议节点");
    require(parsed["protocols"]["proto_b"].IsDefined(), "合并写入应新增当前协议节点");
}

void test_protocol_state_file_roundtrips_elf_path_per_protocol()
{
    YAML::Node root;

    protoscope::ui::storeElfStaticAddressPath(root, "proto_a", "D:/symbols/a.elf");
    protoscope::ui::storeElfStaticAddressPath(root, "proto_b", "D:/symbols/b.elf");

    require(protoscope::ui::restoreElfStaticAddressPath(root, "proto_a") == "D:/symbols/a.elf",
            "proto_a 应恢复自己的 ELF 路径");
    require(protoscope::ui::restoreElfStaticAddressPath(root, "proto_b") == "D:/symbols/b.elf",
            "proto_b 应恢复自己的 ELF 路径");
    require(protoscope::ui::restoreElfStaticAddressPath(root, "proto_c").empty(), "没有保存路径的协议应返回空路径");

    protoscope::ui::storeElfStaticAddressPath(root, "proto_a", "");
    require(protoscope::ui::restoreElfStaticAddressPath(root, "proto_a").empty(),
            "保存空路径应移除当前协议的 ELF 状态");
    require(protoscope::ui::restoreElfStaticAddressPath(root, "proto_b") == "D:/symbols/b.elf",
            "清空 proto_a 不应影响 proto_b 的 ELF 状态");
}

void test_protocol_state_file_replace_failure_keeps_target()
{
    const ScopedTempPath root(makeTempRoot("protocol-state-replace-failure"));
    const auto statePath = root.path() / "config" / "ui" / "protocol-control-state.yaml";
    std::filesystem::create_directories(statePath);

    YAML::Node stateRoot;
    stateRoot["protocols"]["proto_a"]["controls"] = YAML::Node(YAML::NodeType::Map);

    std::string error;
    require(!protoscope::ui::writeProtocolStateRootAtomically(statePath, stateRoot, error),
            "目标是目录时原子替换应失败");
    require(std::filesystem::is_directory(statePath), "替换失败不应破坏原目标");
    require(!error.empty(), "替换失败应返回明确错误");
}
