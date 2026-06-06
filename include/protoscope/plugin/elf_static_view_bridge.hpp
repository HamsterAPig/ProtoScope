#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace protoscope::plugin {

struct ElfStaticAddressEntry {
    std::string label;
    std::string value;
    std::string type;
};

class ElfStaticViewBridge {
public:
    ElfStaticViewBridge();
    ~ElfStaticViewBridge();
    ElfStaticViewBridge(ElfStaticViewBridge&&) noexcept;
    ElfStaticViewBridge& operator=(ElfStaticViewBridge&&) noexcept;
    ElfStaticViewBridge(const ElfStaticViewBridge&) = delete;
    ElfStaticViewBridge& operator=(const ElfStaticViewBridge&) = delete;

    bool loadFile(const std::filesystem::path& path, std::string& error);
    [[nodiscard]] std::vector<ElfStaticAddressEntry> query(std::string queryText, std::size_t limit) const;
    [[nodiscard]] std::optional<ElfStaticAddressEntry> findExactLabel(std::string label) const;
    [[nodiscard]] bool loaded() const;
    [[nodiscard]] const std::string& sourcePath() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace protoscope::plugin
