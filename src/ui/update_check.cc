#include "protoscope/ui/update_check.hpp"

#include "protoscope/build/version.hpp"
#include "protoscope/ui/version_utils.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#endif

#include <optional>
#include <regex>
#include <string>

namespace protoscope::ui {

namespace {

std::optional<std::string> findLatestSemanticTag(std::string_view responseBody) {
    static const std::regex nameRegex(R"json("name"\s*:\s*"([^"]+)")json");
    std::optional<std::string> latestTag;
    std::optional<SemanticVersion> latestVersion;

    for (auto iter = std::regex_iterator(responseBody.begin(), responseBody.end(), nameRegex);
         iter != std::regex_iterator<std::string_view::const_iterator>();
         ++iter) {
        const auto tag = (*iter)[1].str();
        const auto version = parseSemanticTag(tag);
        if (!version.has_value()) {
            continue;
        }
        if (!latestVersion.has_value() || compareSemanticVersion(*latestVersion, *version) < 0) {
            latestVersion = version;
            latestTag = tag;
        }
    }
    return latestTag;
}

std::string toString(std::string_view value) {
    return std::string(value.begin(), value.end());
}

#if defined(_WIN32)
struct WinHttpHandle {
    HINTERNET value{nullptr};

    ~WinHttpHandle() {
        if (value) {
            WinHttpCloseHandle(value);
        }
    }

    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) : value(handle) {}
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    WinHttpHandle(WinHttpHandle&& other) noexcept : value(other.value) {
        other.value = nullptr;
    }
    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
        if (this != &other) {
            if (value) {
                WinHttpCloseHandle(value);
            }
            value = other.value;
            other.value = nullptr;
        }
        return *this;
    }
};

bool httpGetGitHubTags(std::string& responseBody, std::string& error) {
    WinHttpHandle session(WinHttpOpen(L"ProtoScope/1.0",
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS,
                                      0));
    if (!session.value) {
        error = "WinHTTP 会话创建失败";
        return false;
    }
    WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 5000);

    WinHttpHandle connect(WinHttpConnect(session.value, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connect.value) {
        error = "连接 GitHub 失败";
        return false;
    }

    WinHttpHandle request(WinHttpOpenRequest(connect.value,
                                             L"GET",
                                             L"/repos/HamsterAPig/ProtoScope/tags?per_page=30",
                                             nullptr,
                                             WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE));
    if (!request.value) {
        error = "创建 GitHub 请求失败";
        return false;
    }

    constexpr wchar_t kHeaders[] = L"User-Agent: ProtoScope\r\nAccept: application/vnd.github+json\r\n";
    if (!WinHttpSendRequest(request.value,
                            kHeaders,
                            static_cast<DWORD>(-1L),
                            WINHTTP_NO_REQUEST_DATA,
                            0,
                            0,
                            0)
        || !WinHttpReceiveResponse(request.value, nullptr)) {
        error = "发送 GitHub 请求失败";
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (WinHttpQueryHeaders(request.value,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &statusCode,
                            &statusSize,
                            WINHTTP_NO_HEADER_INDEX)
        && statusCode >= 400) {
        error = "GitHub 返回 HTTP " + std::to_string(statusCode);
        return false;
    }

    responseBody.clear();
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.value, &available)) {
            error = "读取 GitHub 响应失败";
            return false;
        }
        if (available == 0) {
            break;
        }
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request.value, chunk.data(), available, &read)) {
            error = "读取 GitHub 响应内容失败";
            return false;
        }
        chunk.resize(read);
        responseBody += chunk;
    }
    return true;
}
#endif

} // namespace

UpdateCheckResult evaluateUpdateCheckTags(std::string_view responseBody, BuildVersionInfo buildInfo) {
    const auto latestTag = findLatestSemanticTag(responseBody);
    if (!latestTag.has_value()) {
        return {UpdateCheckResult::State::Failed, "检查更新失败", "GitHub tags 中未找到 vX.Y.Z 格式版本", ""};
    }

    const auto latestVersion = parseSemanticTag(*latestTag);
    const std::string baseTag = buildInfo.baseTag.empty() ? toString(buildInfo.currentTag) : toString(buildInfo.baseTag);
    const auto currentVersion = parseSemanticTag(baseTag);
    if (!currentVersion.has_value()) {
        return {UpdateCheckResult::State::DevelopmentBuild,
                "当前是开发构建",
                "当前版本没有可比较的 vX.Y.Z 基准，远端最新版本为 " + *latestTag,
                *latestTag};
    }

    const int compare = compareSemanticVersion(*currentVersion, *latestVersion);
    if (compare < 0) {
        return {UpdateCheckResult::State::NewerAvailable,
                "发现新版本",
                "远端最新版本为 " + *latestTag + "，当前版本为 " + toString(buildInfo.version),
                *latestTag};
    }
    if (!buildInfo.isExactTag) {
        return {UpdateCheckResult::State::DevelopmentBuild,
                "当前是开发构建",
                "当前构建基于 " + baseTag + " 之后的提交，远端最新版本为 " + *latestTag,
                *latestTag};
    }

    return {UpdateCheckResult::State::UpToDate,
            "已是最新版本",
            "当前版本 " + toString(buildInfo.version) + " 已是远端最新版本",
            *latestTag};
}

UpdateCheckResult checkForUpdates() {
#if defined(_WIN32)
    std::string body;
    std::string error;
    if (!httpGetGitHubTags(body, error)) {
        return {UpdateCheckResult::State::Failed, "检查更新失败", error, ""};
    }

    return evaluateUpdateCheckTags(body,
                                   BuildVersionInfo{
                                       .version = build::kVersion,
                                       .currentTag = build::kCurrentTag,
                                       .baseTag = build::kBaseTag,
                                       .isExactTag = build::kIsExactTag,
                                   });
#else
    return {UpdateCheckResult::State::Unsupported,
            "暂不支持检查更新",
            "当前平台暂未实现联网检查更新",
            ""};
#endif
}

} // namespace protoscope::ui
