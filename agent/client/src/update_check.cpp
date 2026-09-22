#include "agentxx-client/update_check.h"

#include "utilxx/http_client.h"
#include "utilxx_base/string_util.h"
#include <chrono>
#include <charconv>
#include <cstddef>

namespace agentxx {
namespace client {

namespace {

/// 更新探测的请求超时 (连接与读块各 8 秒): 启动路径上的附加提示, 不应长时间占用
constexpr auto kCheckTimeout = std::chrono::seconds{8};

/// 从 302 响应的 Location 头中取出发布标签与发布页 URL
/// - GitHub 的 `/releases/latest` 返回 `Location: https://github.com/<owner>/<repo>/releases/tag/<tag>`
/// - 取不到 `/tag/` 段时返回空标签 (调用方按失败处理)
///
/// - `return` 标签 (如 "v0.2.0"; 未识别返回空串)
std::string tagFromReleaseUrl(std::string_view releaseUrl) {
    constexpr std::string_view kSegment = "/releases/tag/";
    const auto                 pos      = releaseUrl.find(kSegment);
    if (pos == std::string_view::npos) {
        return {};
    }
    auto tag = releaseUrl.substr(pos + kSegment.size());
    // 去掉查询串/片段 (正常情况下 GitHub 不带, 防御性处理)
    if (const auto cut = tag.find_first_of("?#"); cut != std::string_view::npos) {
        tag = tag.substr(0, cut);
    }
    while (!tag.empty() && tag.back() == '/') {
        tag.remove_suffix(1);
    }
    return std::string{tag};
}

/// 去掉首尾空白 (含 `\r`, 头部值可能带行尾)
std::string_view trimView(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'
                          || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty()
           && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

/// 判断结果中的最新版本是否比当前运行版本新 (填入 [UpdateCheckResult::hasUpdate])
/// - 当前版本取编译期注入的 agentxx::kVersion (如 "0.1.0")
/// - 任一侧解析失败时保持 false (不提示更新, 宁可漏报不误报)
void fillHasUpdate(UpdateCheckResult& result) {
    if (!result.latestVersion.has_value()) {
        return;
    }
    const auto current = parseVersionTag(agentxx::kVersion);
    if (!current.has_value()) {
        return;
    }
    result.hasUpdate = compareVersion(result.latestVersion.value(), current.value()) > 0;
}

} // namespace

std::optional<VersionTriple> parseVersionTag(std::string_view tag) noexcept {
    tag = trimView(tag);
    if (!tag.empty() && (tag.front() == 'v' || tag.front() == 'V')) {
        tag.remove_prefix(1);
    }
    // 预发布/构建元数据后缀不参与比较
    if (const auto cut = tag.find('-'); cut != std::string_view::npos) {
        tag = tag.substr(0, cut);
    }
    if (tag.empty()) {
        return std::nullopt;
    }

    VersionTriple result{0, 0, 0};
    size_t        part    = 0;
    size_t        index   = 0;
    bool          anyDigit = false;
    while (index <= tag.size()) {
        const auto next = tag.find('.', index);
        const auto seg  = tag.substr(index, (next == std::string_view::npos) ? std::string_view::npos
                                                                            : next - index);
        if (seg.empty() || part >= result.size()) {
            return std::nullopt; // 空段 (如 "1..2") 或多于三段
        }
        int  value = 0;
        auto ec    = std::from_chars(seg.data(), seg.data() + seg.size(), value).ec;
        if (ec != std::errc{} || value < 0) {
            return std::nullopt; // 非数字段 (如 "1.x.2" / 预发布标识残留)
        }
        result[part] = value;
        anyDigit     = true;
        ++part;
        if (next == std::string_view::npos) {
            break;
        }
        index = next + 1;
    }
    if (!anyDigit) {
        return std::nullopt;
    }
    return result;
}

int compareVersion(const VersionTriple& a, const VersionTriple& b) noexcept {
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            return (a[i] < b[i]) ? -1 : 1;
        }
    }
    return 0;
}

asio::awaitable<UpdateCheckResult> checkLatestRelease(std::string_view url) {
    UpdateCheckResult result;

    utilxx::RequestConfig cfg;
    // 不跟随重定向: 最新版本标签就在 302 的 Location 里 (见 tagFromReleaseUrl)
    cfg.followRedirect   = 0;
    cfg.connectTimeout   = kCheckTimeout;
    cfg.readChunkTimeout = kCheckTimeout;
    cfg.sslVerify        = std::nullopt;

    utilxx::HeaderMap headers;
    headers.set("Accept", "application/json,text/html;q=0.9");

    auto resp = co_await utilxx::HttpClient::getAsync(url, headers, cfg);
    if (!resp.has_value()) {
        result.error = resp.error().empty() ? std::string{"request failed"} : resp.error();
        co_return result;
    }
    auto& httpResp = resp.value();

    // 1) 重定向 (GitHub /releases/latest): Location -> 发布页 URL 与标签
    if (httpResp.status >= 300 && httpResp.status < 400) {
        auto location = httpResp.findHeader("location");
        if (location.empty()) {
            result.error = "redirect without location header";
            co_return result;
        }
        result.url       = std::string{trimView(location)};
        result.latestTag = tagFromReleaseUrl(result.url);
        if (result.latestTag.empty()) {
            result.error = "unrecognized release location: " + result.url;
            co_return result;
        }
        result.latestVersion = parseVersionTag(result.latestTag);
        if (!result.latestVersion.has_value()) {
            result.error = "unrecognized version tag: " + result.latestTag;
            co_return result;
        }
        result.ok = true;
        fillHasUpdate(result);
        co_return result;
    }

    // 2) 直接返回 JSON (GitHub API): 取 tag_name (html_url 作为发布页 URL)
    if (httpResp.isSuccess()) {
        auto body = httpResp.bodyJson();
        if (body.has_value() && body->is_object()) {
            result.latestTag = body->value("tag_name", std::string{});
            result.url       = body->value("html_url", std::string{});
        }
        if (result.latestTag.empty()) {
            result.error = "response has no release tag";
            co_return result;
        }
        if (result.url.empty()) {
            result.url = std::string{kReleasesUrl};
        }
        result.latestVersion = parseVersionTag(result.latestTag);
        if (!result.latestVersion.has_value()) {
            result.error = "unrecognized version tag: " + result.latestTag;
            co_return result;
        }
        result.ok = true;
        fillHasUpdate(result);
        co_return result;
    }

    result.error = "unexpected status " + std::to_string(httpResp.status);
    co_return result;
}

asio::awaitable<UpdateCheckResult> checkLatestReleaseForCurrentVersion() {
    co_return co_await checkLatestRelease(kLatestReleaseUrl);
}

} // namespace client
} // namespace agentxx
