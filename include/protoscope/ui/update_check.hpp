#pragma once

#include <string>
#include <string_view>

namespace protoscope::ui {

struct UpdateCheckResult {
    enum class State {
        UpToDate,
        NewerAvailable,
        DevelopmentBuild,
        Failed,
        Unsupported,
    };

    State state{State::Failed};
    std::string title;
    std::string message;
    std::string latestTag;
};

struct BuildVersionInfo {
    std::string_view version;
    std::string_view currentTag;
    std::string_view baseTag;
    bool isExactTag{false};
};

// 核心判断与网络请求分离，便于在不访问 GitHub 的单元测试里覆盖版本比较规则。
UpdateCheckResult evaluateUpdateCheckTags(std::string_view responseBody, BuildVersionInfo buildInfo);
UpdateCheckResult checkForUpdates();

} // namespace protoscope::ui
