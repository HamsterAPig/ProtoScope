#include "test_registry.hpp"

#include "protoscope/ui/update_check.hpp"

#include <stdexcept>
#include <string_view>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

constexpr std::string_view kTagsJson = R"json([
  {"name": "v1.0.0"},
  {"name": "v1.4.2"},
  {"name": "v1.4.2-beta"},
  {"name": "not-a-version"},
  {"name": "v1.3.9"}
])json";

} // namespace

void test_update_check_evaluates_newer_version() {
    const auto result = protoscope::ui::evaluateUpdateCheckTags(
        kTagsJson,
        protoscope::ui::BuildVersionInfo{
            .version = "v1.0.0",
            .currentTag = "v1.0.0",
            .baseTag = "",
            .isExactTag = true,
        });

    require(result.state == protoscope::ui::UpdateCheckResult::State::NewerAvailable, "远端高版本应提示发现新版本");
    require(result.latestTag == "v1.4.2", "应选择最高的合法语义版本标签");
}

void test_update_check_reports_up_to_date_for_exact_tag() {
    const auto result = protoscope::ui::evaluateUpdateCheckTags(
        kTagsJson,
        protoscope::ui::BuildVersionInfo{
            .version = "v1.4.2",
            .currentTag = "v1.4.2",
            .baseTag = "",
            .isExactTag = true,
        });

    require(result.state == protoscope::ui::UpdateCheckResult::State::UpToDate, "精确标签等于远端最新版本时应视为最新");
    require(result.latestTag == "v1.4.2", "最新版本标签应保留给 UI 展示");
}

void test_update_check_reports_development_build() {
    const auto result = protoscope::ui::evaluateUpdateCheckTags(
        kTagsJson,
        protoscope::ui::BuildVersionInfo{
            .version = "v1.4.2+abcdef0",
            .currentTag = "",
            .baseTag = "v1.4.2",
            .isExactTag = false,
        });

    require(result.state == protoscope::ui::UpdateCheckResult::State::DevelopmentBuild, "非精确标签构建应标记为开发构建");
    require(result.latestTag == "v1.4.2", "开发构建也应返回远端最新版本");
}

void test_update_check_rejects_response_without_semantic_tags() {
    const auto result = protoscope::ui::evaluateUpdateCheckTags(
        R"json([{"name": "latest"}, {"name": "v1.2"}])json",
        protoscope::ui::BuildVersionInfo{
            .version = "v1.0.0",
            .currentTag = "v1.0.0",
            .baseTag = "",
            .isExactTag = true,
        });

    require(result.state == protoscope::ui::UpdateCheckResult::State::Failed, "没有合法语义版本标签时应失败");
}
