#include "test_registry.hpp"

#include "protoscope/plugin/elf_static_view_bridge.hpp"

#include <elf_static_view/project.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::filesystem::path makeUniqueTempFile(const char* name) {
    return std::filesystem::temp_directory_path() / (std::string(name) + "-" + std::to_string(nowMs()) + ".json");
}

elf_static_view::ExpandedNode makeExpandedNode(std::string path,
                                               std::string displayName,
                                               std::string typeName,
                                               elf_static_view::Availability availability,
                                               std::optional<std::uint64_t> absoluteAddress = std::nullopt) {
    return elf_static_view::ExpandedNode{
        .path = std::move(path),
        .display_name = std::move(displayName),
        .type_name = std::move(typeName),
        .type_id = {},
        .type_kind = elf_static_view::TypeKind::Base,
        .availability = availability,
        .absolute_address = absoluteAddress,
        .relative_offset = std::nullopt,
        .byte_size = std::nullopt,
        .array_count = std::nullopt,
        .array_stride = std::nullopt,
        .depth = 0,
        .children_lazy = false,
        .children = {},
    };
}

elf_static_view::ProjectModel sampleProjectModel() {
    elf_static_view::ProjectModel model;
    model.file = "sample.elf";
    model.expanded.push_back(makeExpandedNode("global.counter",
                                              "counter",
                                              "uint32_t",
                                              elf_static_view::Availability::StaticAddressKnown,
                                              0x20000010ULL));
    model.expanded.push_back(makeExpandedNode("global.a_var_int",
                                              "a_var_int",
                                              "int",
                                              elf_static_view::Availability::StaticAddressKnown,
                                              0x20000000ULL));
    model.expanded.push_back(makeExpandedNode("global.runtime_only",
                                              "runtime_only",
                                              "int",
                                              elf_static_view::Availability::RuntimeOnly));
    return model;
}

} // namespace

void test_elf_static_view_bridge_loads_dump_json_and_queries_symbols() {
    const auto path = makeUniqueTempFile("protoscope-elf-static-view-dump");
    {
        std::ofstream output(path, std::ios::binary);
        output << elf_static_view::render_dump_json(sampleProjectModel());
    }

    protoscope::plugin::ElfStaticViewBridge bridge;
    std::string error;
    require(bridge.loadFile(path, error), "桥接层应能加载 dump JSON");
    const auto results = bridge.query("counter", 64);
    require(results.size() == 1, "应查到 1 个静态地址结果");
    require(results[0].label == "global.counter", "结果 label 应来自 ElfStaticView key");
    require(results[0].value == "0x20000010", "结果 value 应格式化为十六进制字符串");
    require(results[0].type == "uint32_t", "结果 type 应来自 ElfStaticView value_type");

    const auto wildcardResults = bridge.query("a_var*", 64);
    require(wildcardResults.size() == 1, "应兼容 Lua 下拉输入的通配符后缀");
    require(wildcardResults[0].label == "global.a_var_int", "a_var* 应命中 a_var_int");
    require(wildcardResults[0].value == "0x20000000", "a_var_int 地址应格式化为十六进制字符串");

    std::filesystem::remove(path);
}

void test_elf_static_view_bridge_keeps_old_model_on_load_failure() {
    const auto path = makeUniqueTempFile("protoscope-elf-static-view-keep-old");
    {
        std::ofstream output(path, std::ios::binary);
        output << elf_static_view::render_dump_json(sampleProjectModel());
    }

    protoscope::plugin::ElfStaticViewBridge bridge;
    std::string error;
    require(bridge.loadFile(path, error), "桥接层应先加载有效 JSON");
    require(!bridge.loadFile(path.parent_path() / "missing.json", error), "加载缺失文件应失败");
    require(!bridge.query("counter", 64).empty(), "加载失败后应保留旧模型");

    std::filesystem::remove(path);
}
