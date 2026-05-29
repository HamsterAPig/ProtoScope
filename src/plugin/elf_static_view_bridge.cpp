#include "protoscope/plugin/elf_static_view_bridge.hpp"

#include <elf_static_view/project.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace protoscope::plugin {

namespace {

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("打开文件失败: " + path.string());
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string lowerExtension(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return extension;
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

std::optional<elf_static_view::ProjectModel> parseJsonModel(const std::string& text, std::string& error) {
    try {
        return elf_static_view::parse_snapshot_json(text).model;
    } catch (const std::exception& snapshotError) {
        try {
            return elf_static_view::parse_dump_json(text);
        } catch (const std::exception& dumpError) {
            error = std::string("解析 snapshot JSON 失败: ") + snapshotError.what()
                  + "；解析 dump JSON 失败: " + dumpError.what();
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

        std::optional<elf_static_view::ProjectModel> loadedModel;
        if (lowerExtension(path) == ".json") {
            const std::string text = readTextFile(path);
            loadedModel = parseJsonModel(text, error);
            if (!loadedModel.has_value()) {
                return false;
            }
        } else {
            elf_static_view::ProjectLoader loader;
            elf_static_view::ScanOptions options;
            options.include_runtime_only = false;
            options.load_policy.exclude_runtime_only_variables = true;
            options.load_policy.static_storage_only = true;
            options.load_policy.lazy_expand_children = true;
            loadedModel = loader.scan(path.string(), options);
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
    options.only_static_known = true;
    options.include_runtime_only = false;
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
