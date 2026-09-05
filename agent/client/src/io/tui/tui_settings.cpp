#include "agentxx-client/io/tui/framework/tui_settings.h"

#include "agentxx/util/env.h"
#include "agentxx/util/string_util.h"
#include <clocale>
#include <optional>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

/// 解析单个语言标签/区域标识符并尝试在已支持语言中匹配
///
/// - 支持常见格式:
///   - POSIX: "zh_CN.UTF-8", "zh_CN", "en_US.UTF-8", "en_GB"
///   - BCP-47: "zh-CN", "zh-Hans-CN", "zh-Hant-TW", "en-US"
///   - 简写与别名: "zh", "chinese", "en", "english"
/// - 处理流程:
///   1) 去除前后空白字符;
///   2) 截掉修饰符 (@pinyin 等) 与字符集编码 (.UTF-8 等);
///   3) 统一转小写并将 '_' 归一化为 '-';
///   4) 过滤无效的环境标记 ("c", "posix");
///   5) 匹配中文 -> ZhCn;
///   6) 匹配英文 -> EnUs;
///   7) 匹配其他非中文系统语言 (如 ja, ko, fr, de, es, ru 等) -> 回退到国际通用语言 EnUs;
///   8) 无法识别返回 nullopt (由调用方进一步处理或兜底)。
std::optional<TuiLanguage> matchSingleLocale(std::string_view raw) noexcept {
    // 1. 去除首尾空白
    while (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t')) {
        raw.remove_prefix(1);
    }
    while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t')) {
        raw.remove_suffix(1);
    }
    if (raw.empty()) {
        return std::nullopt;
    }

    // 2. 截断修饰符 (如 zh_CN.UTF-8@pinyin -> zh_CN.UTF-8)
    const auto atPos = raw.find('@');
    if (atPos != std::string_view::npos) {
        raw = raw.substr(0, atPos);
    }
    // 截断编码 (如 zh_CN.UTF-8 -> zh_CN)
    const auto dotPos = raw.find('.');
    if (dotPos != std::string_view::npos) {
        raw = raw.substr(0, dotPos);
    }

    if (raw.empty()) {
        return std::nullopt;
    }

    // 3. 规范化: 转小写, 并将 '_' 替换为 '-'
    std::string norm;
    norm.reserve(raw.size());
    for (char c : raw) {
        if (c == '_') {
            norm.push_back('-');
        } else {
            norm.push_back(static_cast<char>(agentxx::util::charToLower(static_cast<unsigned char>(c))));
        }
    }

    // 4. 忽略纯 C / POSIX (表示未配置特定本地化)
    if (norm == "c" || norm == "posix") {
        return std::nullopt;
    }

    // 5. 中文匹配:
    // zh, zh-cn, zh-hans, zh-hant, zh-tw, zh-hk, zh-sg, 或包含 chinese
    if (norm == "zh" || norm.starts_with("zh-") || norm.find("chinese") != std::string::npos) {
        return TuiLanguage::ZhCn;
    }

    // 6. 英文匹配:
    // en, en-us, en-gb, en-ca, en-au, 或包含 english
    if (norm == "en" || norm.starts_with("en-") || norm.find("english") != std::string::npos) {
        return TuiLanguage::EnUs;
    }

    // 7. 其它常见语言 (非中文环境):
    // 如 ja-jp, ko-kr, fr-fr, de-de, es-es, ru-ru, it-it, pt-br 等,
    // 或者符合 ISO 639 语言代码格式 (2-3 个字母前缀后接 '-' 或串结束)。
    // 在当前已支持列表 [ZhCn, EnUs] 中, 非中文环境统一回退到国际通用语言 English (EnUs)。
    size_t alphaLen = 0;
    while (alphaLen < norm.size() && norm[alphaLen] >= 'a' && norm[alphaLen] <= 'z') {
        ++alphaLen;
    }
    if ((alphaLen == 2 || alphaLen == 3) && (alphaLen == norm.size() || norm[alphaLen] == '-')) {
        return TuiLanguage::EnUs;
    }

    return std::nullopt;
}

} // namespace

TuiLanguage matchSupportedLanguage(std::string_view localeStr) noexcept {
    // 支持按优先级组合的多条目列表 (如 GNU LANGUAGE="zh_CN:zh:en_US:en" 或带分号)
    while (!localeStr.empty()) {
        const auto sepPos = localeStr.find_first_of(":;,");
        const auto item   = (sepPos == std::string_view::npos)
                                ? localeStr
                                : localeStr.substr(0, sepPos);
        localeStr = (sepPos == std::string_view::npos)
                        ? std::string_view{}
                        : localeStr.substr(sepPos + 1);

        const auto matched = matchSingleLocale(item);
        if (matched.has_value()) {
            return *matched;
        }
    }

    // 无法识别或未指定时, 回退默认语言 (简体中文)
    return TuiLanguage::ZhCn;
}

std::string detectSystemLocale() {
    // 1. 优先读取环境变量 (跨平台: POSIX 与已配置环境的 Windows 均生效)
    // 优先级参照 POSIX gettext: LC_ALL -> LC_MESSAGES -> LANG -> LANGUAGE
    const char* const envVars[] = {"LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"};
    for (const char* var : envVars) {
        if (auto val = agentxx::util::getEnv(var); val && !val->empty()) {
            const std::string& s = *val;
            // 过滤无意义的空或纯 C/POSIX 设置
            if (s != "C" && s != "POSIX" && s != "c" && s != "posix") {
                return s;
            }
        }
    }

#ifdef _WIN32
    // 2. Windows 平台特化探测 (当环境变量未设置时)
    // Vista / Win7 / Win10+ 均支持 GetUserDefaultLocaleName
    wchar_t wname[LOCALE_NAME_MAX_LENGTH] = {0};
    if (::GetUserDefaultLocaleName(wname, LOCALE_NAME_MAX_LENGTH) > 0) {
        std::string localeName;
        for (int i = 0; wname[i] != L'\0'; ++i) {
            localeName.push_back(static_cast<char>(wname[i]));
        }
        if (!localeName.empty()) {
            return localeName;
        }
    }
    // 兜底: GetUserDefaultUILanguage
    const LANGID langId  = ::GetUserDefaultUILanguage();
    const WORD   priLang = PRIMARYLANGID(langId);
    if (priLang == LANG_CHINESE) {
        return "zh-CN";
    } else if (priLang == LANG_ENGLISH) {
        return "en-US";
    }
#else
    // 3. POSIX 系统兜底: 尝试 std::setlocale 查询系统默认配置
    const char* cur = std::setlocale(LC_MESSAGES, "");
    if (!cur || std::string_view(cur).empty() || std::string_view(cur) == "C"
        || std::string_view(cur) == "POSIX") {
        cur = std::setlocale(LC_ALL, "");
    }
    std::string result;
    if (cur && !std::string_view(cur).empty() && std::string_view(cur) != "C"
        && std::string_view(cur) != "POSIX") {
        result = cur;
    }
    // 探测完毕后恢复为 "C", 避免影响全局数字/时间格式化行为
    std::setlocale(LC_ALL, "C");
    if (!result.empty()) {
        return result;
    }
#endif

    return {};
}

TuiLanguage detectSystemLanguage() {
    return matchSupportedLanguage(detectSystemLocale());
}
