#include "test_registry.hpp"

#include "protoscope/plugin/elf_static_view_bridge.hpp"

#include <elf_static_view/project.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

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

std::filesystem::path makeUniqueTempFile(const char* name, const std::string& extension = ".json") {
    return std::filesystem::temp_directory_path() / (std::string(name) + "-" + std::to_string(nowMs()) + extension);
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
        .export_path = {},
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
    auto objAdc = makeExpandedNode("global.objADC",
                                   "objADC",
                                   "AdcState",
                                   elf_static_view::Availability::StaticAddressKnown,
                                   0x20000100ULL);
    objAdc.type_kind = elf_static_view::TypeKind::Struct;
    objAdc.children.push_back(makeExpandedNode("global.objADC.member",
                                               "member",
                                               "uint16_t",
                                               elf_static_view::Availability::StaticAddressKnown,
                                               0x20000104ULL));
    model.expanded.push_back(std::move(objAdc));
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

void test_elf_static_view_bridge_queries_flattened_composite_members() {
    const auto path = makeUniqueTempFile("protoscope-elf-static-view-composite");
    {
        std::ofstream output(path, std::ios::binary);
        output << elf_static_view::render_dump_json(sampleProjectModel());
    }

    protoscope::plugin::ElfStaticViewBridge bridge;
    std::string error;
    require(bridge.loadFile(path, error), "桥接层应能加载含复合对象的 dump JSON");

    const auto memberResults = bridge.query("member", 64);
    require(memberResults.size() == 1, "开启展开后应能按成员名查询复合对象成员");
    require(memberResults[0].label == "global.objADC.member", "成员查询应返回完整成员路径");
    require(memberResults[0].value == "0x20000104", "成员地址应来自展开后的复合成员");
    require(memberResults[0].type == "uint16_t", "成员类型应来自展开后的复合成员");

    const auto objectPrefixResults = bridge.query("objADC", 64);
    const auto memberIt = std::find_if(objectPrefixResults.begin(),
                                       objectPrefixResults.end(),
                                       [](const protoscope::plugin::ElfStaticAddressEntry& entry) {
                                           return entry.label == "global.objADC.member";
                                       });
    require(memberIt != objectPrefixResults.end(), "按对象名前缀查询时应包含展开后的成员地址");

    std::filesystem::remove(path);
}

void test_elf_static_view_bridge_loads_private_binary_without_extension() {
    const auto path = makeUniqueTempFile("protoscope-elf-static-view-private", "");
    {
        elf_static_view::ProjectSnapshot snapshot;
        snapshot.source_file = "sample.elf";
        snapshot.model = sampleProjectModel();
        elf_static_view::ExportDocument document{
            elf_static_view::ExportPayloadKind::FullSnapshot,
            std::move(snapshot),
        };
        elf_static_view::ExportOptions options;
        options.format = elf_static_view::ExportFormat::BinaryPrivate;

        std::ofstream output(path, std::ios::binary);
        output << elf_static_view::render_export_document(document, options);
    }

    protoscope::plugin::ElfStaticViewBridge bridge;
    std::string error;
    require(bridge.loadFile(path, error), "桥接层应按内容加载无后缀私有二进制导出");
    const auto results = bridge.query("counter", 64);
    require(results.size() == 1, "私有二进制导出应能查询静态地址");
    require(results[0].value == "0x20000010", "私有二进制导出的地址应保持一致");

    std::filesystem::remove(path);
}

void test_elf_static_view_bridge_loads_variable_summary_export() {
    const auto path = makeUniqueTempFile("protoscope-elf-static-view-summary", ".esv");
    {
        elf_static_view::ExportOptions options;
        options.format = elf_static_view::ExportFormat::JsonCompact;
        options.payload_kind = elf_static_view::ExportPayloadKind::VariableSummary;
        auto lightweightExport = elf_static_view::build_lightweight_export(sampleProjectModel(), options);
        const auto summaryVariable = std::find_if(lightweightExport.variables.begin(),
                                                  lightweightExport.variables.end(),
                                                  [](const elf_static_view::LightweightVariableRecord& variable) {
                                                      return variable.path == "global.a_var_int";
                                                  });
        require(summaryVariable != lightweightExport.variables.end(), "轻量变量摘要应导出完整变量路径");
        require(summaryVariable->name == "global.a_var_int", "v0.3.5 轻量变量摘要 name 应使用完整路径");
        elf_static_view::ExportDocument document{
            elf_static_view::ExportPayloadKind::VariableSummary,
            std::move(lightweightExport),
        };

        std::ofstream output(path, std::ios::binary);
        output << elf_static_view::render_export_document(document, options);
    }

    protoscope::plugin::ElfStaticViewBridge bridge;
    std::string error;
    require(bridge.loadFile(path, error), "桥接层应加载 ElfStaticView 轻量变量摘要导出");
    const auto results = bridge.query("a_var", 64);
    require(results.size() == 1, "轻量变量摘要应能进入静态地址查询");
    require(results[0].label == "global.a_var_int", "轻量变量摘要应保留变量路径");
    require(results[0].value == "0x20000000", "轻量变量摘要应保留静态地址");

    std::filesystem::remove(path);
}

void test_elf_static_view_bridge_keeps_old_model_on_load_failure() {
    const auto path = makeUniqueTempFile("protoscope-elf-static-view-keep-old");
    const auto invalidPath = makeUniqueTempFile("protoscope-elf-static-view-invalid", ".txt");
    {
        std::ofstream output(path, std::ios::binary);
        output << elf_static_view::render_dump_json(sampleProjectModel());
    }
    {
        std::ofstream output(invalidPath, std::ios::binary);
        output << "not an elf or export";
    }

    protoscope::plugin::ElfStaticViewBridge bridge;
    std::string error;
    require(bridge.loadFile(path, error), "桥接层应先加载有效 JSON");
    require(!bridge.loadFile(path.parent_path() / "missing.json", error), "加载缺失文件应失败");
    require(!bridge.loadFile(invalidPath, error), "加载无效内容应失败");
    require(error.find("ElfStaticView") != std::string::npos, "错误信息应包含数据导入上下文");
    require(error.find("ELF 扫描失败") != std::string::npos, "错误信息应包含 ELF 扫描上下文");
    require(!bridge.query("counter", 64).empty(), "加载失败后应保留旧模型");

    std::filesystem::remove(path);
    std::filesystem::remove(invalidPath);
}
