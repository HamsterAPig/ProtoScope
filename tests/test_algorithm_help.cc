#include "protoscope/ui/algorithm_help.hpp"

#include "test_registry.hpp"

#include <algorithm>
#include <stdexcept>
#include <string_view>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool hasTitle(std::string_view query, std::string_view title)
{
    const auto matches = protoscope::ui::findAlgorithmHelpMatches(query);
    const auto entries = protoscope::ui::algorithmHelpEntries();
    return std::any_of(matches.begin(), matches.end(), [&](std::size_t index) {
        return index < entries.size() && entries[index].title == title;
    });
}

} // namespace

void test_algorithm_help_search_empty_query_has_no_match()
{
    require(protoscope::ui::findAlgorithmHelpMatches("").empty(), "空查询不应产生跳转命中");
    require(protoscope::ui::findAlgorithmHelpMatches("   \t ").empty(), "空白查询不应产生跳转命中");
}

void test_algorithm_help_search_chinese_keyword()
{
    require(hasTitle("相位", "FFT 相位与角度"), "中文关键词应命中相位条目");
}

void test_algorithm_help_search_english_case_insensitive()
{
    require(hasTitle("fft", "FFT 输入窗口与横轴"), "英文小写 fft 应命中 FFT 条目");
    require(hasTitle("TIMEOUT", "请求链路与原始回放"), "英文大写 TIMEOUT 应大小写不敏感");
}

void test_algorithm_help_search_multiple_terms_require_all()
{
    require(hasTitle("FFT dB", "FFT 幅值单位"), "多个关键词全部命中时应返回对应条目");
    require(!hasTitle("FFT CRC", "FFT 幅值单位"), "缺少任一关键词时不应命中该条目");
}

void test_algorithm_help_search_no_match()
{
    require(protoscope::ui::findAlgorithmHelpMatches("not-present-keyword").empty(), "不存在关键词应无命中");
}

void test_algorithm_help_search_navigation_wraps()
{
    const auto matches = protoscope::ui::findAlgorithmHelpMatches("FFT");
    require(matches.size() >= 3U, "FFT 至少应命中输入、幅值和相位条目");

    const std::size_t first =
        protoscope::ui::nextAlgorithmHelpMatchOrdinal(matches, protoscope::ui::kNoAlgorithmHelpMatch);
    require(first == 0U, "首次下一条应定位到第一个命中");
    const std::size_t second = protoscope::ui::nextAlgorithmHelpMatchOrdinal(matches, first);
    require(second == 1U, "下一条应前进到第二个命中");
    const std::size_t wrappedNext = protoscope::ui::nextAlgorithmHelpMatchOrdinal(matches, matches.size() - 1U);
    require(wrappedNext == 0U, "最后一条的下一条应循环到第一条");
    const std::size_t wrappedPrevious = protoscope::ui::previousAlgorithmHelpMatchOrdinal(matches, 0U);
    require(wrappedPrevious == matches.size() - 1U, "第一条的上一条应循环到最后一条");
}

void test_algorithm_help_search_fft_unit_keywords()
{
    require(hasTitle("actualValue ratio", "FFT 幅值单位"), "FFT 幅值条目应包含 actualValue 和 ratio");
    require(hasTitle("dB 1e-12", "FFT 幅值单位"), "FFT 幅值条目应包含 dB 下限公式关键字");
    require(hasTitle("% 基波", "FFT 幅值单位"), "FFT 幅值条目应包含 % 基波说明");
}
