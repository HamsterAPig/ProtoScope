#include "protoscope/plugin/elf_static_view_bridge.hpp"

#include <elf_static_view/project.hpp>

#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace protoscope::plugin {

namespace {

constexpr std::string_view kElfStaticViewPrivateMagic = "ESVEXP01";

std::string readFileBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("打开文件失败: " + path.string());
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string readFilePrefix(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("打开文件失败: " + path.string());
    }

    std::string prefix(64, '\0');
    input.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
    prefix.resize(static_cast<std::size_t>(input.gcount()));
    return prefix;
}

bool isWhitespace(char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n'
        || value == '\f' || value == '\v';
}

bool looksLikeElfStaticViewData(std::string_view prefix) {
    if (prefix.starts_with(kElfStaticViewPrivateMagic)) {
        return true;
    }
    for (char value : prefix) {
        if (isWhitespace(value)) {
            continue;
        }
        return value == '{' || value == '[';
    }
    return false;
}

std::string formatAddress(std::uint64_t value) {
    std::ostringstream builder;
    builder << "0x" << std::uppercase << std::hex << value;
    return builder.str();
}

std::string normalizeNameQuery(std::string queryText) {
    for (char& ch : queryText) {
        if (ch == '*' || ch == '?') {
            ch = ' ';
        }
    }
    return queryText;
}

std::optional<elf_static_view::ProjectModel> parseDataModel(const std::string& bytes,
                                                            const std::filesystem::path& path,
                                                            std::string& error) {
    try {
        auto importedData = elf_static_view::import_project_data_bytes(bytes, path.string());
        return std::move(importedData.snapshot.model);
    } catch (const std::exception& importError) {
        try {
            return elf_static_view::parse_dump_json(bytes);
        } catch (const std::exception& dumpError) {
            error = std::string("导入 ElfStaticView 数据失败: ") + importError.what()
                  + "；解析旧版 dump JSON 失败: " + dumpError.what();
            return std::nullopt;
        }
    }
}

} // namespace

struct ElfStaticViewBridge::Impl {
    std::optional<elf_static_view::ProjectModel> model;
    std::unique_ptr<elf_static_view::StaticAddressQuerySession> session;
    std::string sourcePath;
};

ElfStaticViewBridge::ElfStaticViewBridge()
    : impl_(std::make_unique<Impl>()) {}

ElfStaticViewBridge::~ElfStaticViewBridge() = default;
ElfStaticViewBridge::ElfStaticViewBridge(ElfStaticViewBridge&&) noexcept = default;
ElfStaticViewBridge& ElfStaticViewBridge::operator=(ElfStaticViewBridge&&) noexcept = default;

bool ElfStaticViewBridge::loadFile(const std::filesystem::path& path, std::string& error) {
    error.clear();
    try {
        if (!std::filesystem::exists(path)) {
            error = "文件不存在: " + path.string();
            return false;
        }

        const std::string prefix = readFilePrefix(path);
        std::optional<elf_static_view::ProjectModel> loadedModel;
        std::string dataError;
        if (looksLikeElfStaticViewData(prefix)) {
            const std::string bytes = readFileBytes(path);
            loadedModel = parseDataModel(bytes, path, dataError);
        } else {
            dataError = "文件内容不像 ElfStaticView 导出数据";
        }

        if (!loadedModel.has_value()) {
            elf_static_view::ProjectLoader loader;
            elf_static_view::ScanOptions options;
            options.include_runtime_only = false;
            options.load_policy.exclude_runtime_only_variables = true;
            options.load_policy.static_storage_only = true;
            options.load_policy.lazy_expand_children = true;
            try {
                loadedModel = loader.scan(path.string(), options);
            } catch (const std::exception& scanError) {
                error = dataError + "；ELF 扫描失败: " + scanError.what();
                return false;
            }
        }

        // 核心流程：只有新模型完整加载成功后才替换旧 session，失败时保留旧查询上下文。
        impl_->model = std::move(loadedModel);
        impl_->session = std::make_unique<elf_static_view::StaticAddressQuerySession>(*impl_->model);
        impl_->sourcePath = path.string();
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

std::vector<ElfStaticAddressEntry> ElfStaticViewBridge::query(std::string queryText, std::size_t limit) const {
    if (!impl_->session || limit == 0) {
        return {};
    }

    elf_static_view::StaticAddressQueryOptions options;
    // 核心流程：Lua 下拉输入习惯用 `a_var*` 这类通配符，ElfStaticView 当前按普通子串 token 查询，
    // 因此在宿主桥接层把通配符转为空白，让 `a_var*` 能命中 `a_var_int`。
    options.name_query_text = normalizeNameQuery(std::move(queryText));
    // 核心流程：结构体/数组成员常只有静态布局地址，桥接层需要保留这类可计算地址候选。
    options.only_static_known = false;
    options.include_runtime_only = false;
    options.flatten_composite_members = true;
    options.max_array_elements = limit;

    std::vector<ElfStaticAddressEntry> entries;
    const auto results = impl_->session->query(options);
    entries.reserve(std::min(limit, results.size()));
    for (const auto& result : results) {
        if (entries.size() >= limit) {
            break;
        }
        entries.push_back(ElfStaticAddressEntry{
            .label = result.key,
            .value = formatAddress(result.value),
            .type = result.value_type,
        });
    }
    return entries;
}

bool ElfStaticViewBridge::loaded() const {
    return impl_->session != nullptr;
}

const std::string& ElfStaticViewBridge::sourcePath() const {
    return impl_->sourcePath;
}

} // namespace protoscope::plugin
