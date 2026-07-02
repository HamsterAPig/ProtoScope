#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace protoscope::ui {

struct AlgorithmHelpEntry {
    std::string_view title;
    std::string_view body;
    std::string_view keywords;
};

constexpr std::size_t kNoAlgorithmHelpMatch = static_cast<std::size_t>(-1);

// UI 内部可测试 helper：只维护内置算法手册内容与检索，不作为脚本或插件扩展点。
std::span<const AlgorithmHelpEntry> algorithmHelpEntries();
std::vector<std::size_t> findAlgorithmHelpMatches(std::string_view query);
std::size_t nextAlgorithmHelpMatchOrdinal(std::span<const std::size_t> matches, std::size_t currentOrdinal);
std::size_t previousAlgorithmHelpMatchOrdinal(std::span<const std::size_t> matches, std::size_t currentOrdinal);

} // namespace protoscope::ui
