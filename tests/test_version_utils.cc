#include "protoscope/ui/version_utils.hpp"

#include "test_registry.hpp"

#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

void test_gui_runtime_version_utils()
{
    using protoscope::ui::SemanticVersion;

    const auto v123 = protoscope::ui::parseSemanticTag("v1.2.3");
    require(v123.has_value(), "合法语义版本应可解析");
    require(v123->major == 1 && v123->minor == 2 && v123->patch == 3, "版本字段解析错误");

    const auto v010 = protoscope::ui::parseSemanticTag("v0.1.0");
    require(v010.has_value(), "第二个合法语义版本应可解析");
    require(protoscope::ui::compareSemanticVersion(*v123, *v010) > 0, "版本比较应按 major/minor/patch 递进");
    require(protoscope::ui::compareSemanticVersion(*v010, *v123) < 0, "版本比较方向错误");

    require(!protoscope::ui::parseSemanticTag("1.2.3").has_value(), "缺少 v 前缀应拒绝");
    require(!protoscope::ui::parseSemanticTag("v1.2").has_value(), "缺少 patch 应拒绝");
    require(!protoscope::ui::parseSemanticTag("v1.2.3-beta").has_value(), "附加后缀应拒绝");

    const SemanticVersion same{.major = 2, .minor = 4, .patch = 6};
    require(protoscope::ui::compareSemanticVersion(same, same) == 0, "相同版本应视为相等");
}
