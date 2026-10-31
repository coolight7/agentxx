#include "chatbot_time.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>

namespace agentxx {
namespace chatbot {

namespace {

bool isDigitChar(char c) {
    return c >= '0' && c <= '9';
}

/// 去掉首尾空白 (空格 / 制表 / 换行 / 回车)
std::string_view trimmedView(std::string_view text) {
    size_t begin = 0;
    size_t end   = text.size();
    while (begin < end) {
        const char c = text[begin];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++begin;
            continue;
        }
        break;
    }
    while (end > begin) {
        const char c = text[end - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            --end;
            continue;
        }
        break;
    }
    return text.substr(begin, end - begin);
}

std::string lowerCopy(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        } else {
            out.push_back(c);
        }
    }
    return out;
}

/// 从 `text[pos, pos+len)` 读十进制数字 (不足 len 位或含非数字返回 -1)
int readFixedDigits(std::string_view text, size_t pos, size_t len) {
    if (pos + len > text.size()) {
        return -1;
    }
    int value = 0;
    for (size_t i = 0; i < len; ++i) {
        const char c = text[pos + i];
        if (!isDigitChar(c)) {
            return -1;
        }
        value = value * 10 + (c - '0');
    }
    return value;
}

/// 公历日期 → 距 1970-01-01 的天数 (负数表示 1970 之前)
///
/// 用不到系统时区: 只要能把年月日折算成天数, 就能自己算时间点的秒数。
int64_t daysFromCivil(int year, unsigned month, unsigned day) {
    int64_t       y   = year;
    y -= month <= 2 ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy
        = (153u * (month > 2 ? month - 3 : month + 9) + 2u) / 5u + day - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

/// `std::tm` 的年月日时分秒按 UTC 折算成秒 (忽略 `tm_wday` / `tm_isdst` 等派生字段)
int64_t secondsOfTm(const std::tm& tm) {
    const int64_t days
        = daysFromCivil(tm.tm_year + 1900, static_cast<unsigned>(tm.tm_mon + 1),
                        static_cast<unsigned>(tm.tm_mday));
    return days * 86400 + tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
}

/// `±[H]H[:]?[MM]` 形态的偏移 → 分钟
bool parseNumericOffset(std::string_view text, int32_t& out, std::string& err) {
    size_t pos  = 0;
    int    sign = 1;
    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
        sign = text[pos] == '-' ? -1 : 1;
        ++pos;
    }
    int    hours    = 0;
    size_t hourDig  = 0;
    while (pos < text.size() && isDigitChar(text[pos]) && hourDig < 2) {
        hours = hours * 10 + (text[pos] - '0');
        ++pos;
        ++hourDig;
    }
    if (hourDig == 0) {
        err = "时区偏移缺小时数 (示例: +08:00)";
        return false;
    }
    bool   hadColon = false;
    if (pos < text.size() && text[pos] == ':') {
        hadColon = true;
        ++pos;
    }
    int    minutes   = 0;
    size_t minuteDig = 0;
    while (pos < text.size() && isDigitChar(text[pos])) {
        if (minuteDig < 2) {
            minutes = minutes * 10 + (text[pos] - '0');
        }
        ++pos;
        ++minuteDig;
    }
    if (minuteDig > 2 || (hadColon && minuteDig != 2)) {
        err = "时区偏移的分钟数必须是两位 (示例: +08:00)";
        return false;
    }
    if (pos != text.size()) {
        err = "时区偏移里有无法识别的字符 (示例: +08:00)";
        return false;
    }
    if (minutes > 59) {
        err = "时区偏移的分钟数必须在 0~59";
        return false;
    }
    const int total = hours * 60 + minutes;
    if (total > 14 * 60) {
        err = "时区偏移超出范围 (最大 UTC+14:00)";
        return false;
    }
    out = static_cast<int32_t>(sign * total);
    return true;
}

/// 看起来像 IANA 时区名的字符集 (`Asia/Shanghai` / `Etc/GMT-8` / `UTC`)
bool looksLikeZoneName(std::string_view text) {
    if (text.size() < 3) {
        return false;
    }
    for (char c : text) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || isDigitChar(c)
                        || c == '/' || c == '_' || c == '-' || c == '+';
        if (!ok) {
            return false;
        }
    }
    return true;
}

} // namespace

int64_t systemNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()
    )
        .count();
}

Rfc3339Time parseRfc3339Time(std::string_view text, int32_t defaultOffsetMinutes) {
    Rfc3339Time out;
    const auto   body = trimmedView(text);
    if (body.size() < 19) {
        return out;
    }
    // 前 19 个字符固定是 `YYYY-MM-DDTHH:MM:SS` (分隔符 T 与空格都认)
    const int year   = readFixedDigits(body, 0, 4);
    const int month  = readFixedDigits(body, 5, 2);
    const int day    = readFixedDigits(body, 8, 2);
    const int hour   = readFixedDigits(body, 11, 2);
    const int minute = readFixedDigits(body, 14, 2);
    const int second = readFixedDigits(body, 17, 2);
    if (year < 1970 || year > 9999 || month < 1 || month > 12 || day < 1 || day > 31
        || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) {
        return out;
    }
    size_t pos = 19;
    // 小数秒 (`.123` / `,123`): 只取前三位当毫秒, 多余的精度直接丢掉
    int32_t millis = 0;
    if (pos < body.size() && (body[pos] == '.' || body[pos] == ',')) {
        ++pos;
        int    digits = 0;
        while (pos < body.size() && isDigitChar(body[pos])) {
            if (digits < 3) {
                millis = millis * 10 + (body[pos] - '0');
                ++digits;
            }
            ++pos;
        }
        while (digits > 0 && digits < 3) {
            millis *= 10;
            ++digits;
        }
    }
    // 时区后缀: `Z` / `±HH:MM` / `±HHMM` / `±HH`; 没有后缀 = 平台没给时区
    int32_t offset = kUnknownTzOffset;
    if (pos < body.size()) {
        const char c = body[pos];
        if (c == 'Z' || c == 'z') {
            offset = 0;
            ++pos;
        } else if (c == '+' || c == '-') {
            int32_t    parsed = 0;
            std::string ignored;
            const auto tail = body.substr(pos);
            if (!parseNumericOffset(tail, parsed, ignored)) {
                return out;
            }
            offset = parsed;
            pos    = body.size();
        }
    }
    if (pos != body.size()) {
        // 尾部还有看不懂的东西 (例如 "+08:00[Asia/Shanghai]"): 不当成合法时间
        return out;
    }
    const int32_t usedOffset = offset != kUnknownTzOffset ? offset : defaultOffsetMinutes;
    const int64_t baseSec = daysFromCivil(year, static_cast<unsigned>(month),
                                          static_cast<unsigned>(day))
                                * 86400
                            + hour * 3600 + minute * 60 + second;
    // 时间点 = 本地钟点 - 偏移 (东八区 12:00 就是 UTC 04:00)
    const int64_t seconds = baseSec - static_cast<int64_t>(usedOffset) * 60;
    if (seconds <= 0) {
        return out;
    }
    out.ms            = seconds * 1000 + millis;
    out.offsetMinutes = offset;
    return out;
}

bool parseTimezoneSpec(std::string_view text, TimezoneSpec& out, std::string& err) {
    err.clear();
    const auto body = trimmedView(text);
    if (body.empty()) {
        out      = TimezoneSpec{};
        out.kind = TimezoneSpec::Kind::Auto;
        return true;
    }
    const std::string lower = lowerCopy(body);
    if (lower == "auto" || lower == "platform") {
        out = TimezoneSpec{};
        return true;
    }
    if (lower == "system" || lower == "local" || lower == "localtime" || lower == "os") {
        out      = TimezoneSpec{};
        out.kind = TimezoneSpec::Kind::System;
        return true;
    }
    if (lower == "utc" || lower == "gmt" || lower == "z") {
        out               = TimezoneSpec{};
        out.kind          = TimezoneSpec::Kind::FixedOffset;
        out.offsetMinutes = 0;
        return true;
    }
    // `UTC+8` / `GMT-05:00`: 去掉前缀后当偏移解析
    std::string_view rest = body;
    if (lower.rfind("utc", 0) == 0 || lower.rfind("gmt", 0) == 0) {
        const auto tail = body.substr(3);
        if (!tail.empty() && (tail.front() == '+' || tail.front() == '-'
                              || isDigitChar(tail.front()))) {
            rest = tail;
        }
    }
    if (!rest.empty() && (rest.front() == '+' || rest.front() == '-')) {
        int32_t minutes = 0;
        if (!parseNumericOffset(rest, minutes, err)) {
            return false;
        }
        out               = TimezoneSpec{};
        out.kind          = TimezoneSpec::Kind::FixedOffset;
        out.offsetMinutes = minutes;
        return true;
    }
    // 纯数字 = 小时数 (`8` 就是 UTC+8)
    bool allDigits = !rest.empty();
    for (char c : rest) {
        if (!isDigitChar(c)) {
            allDigits = false;
            break;
        }
    }
    if (allDigits) {
        if (rest.size() > 2) {
            err = "时区小时数最多两位 (示例: +08:00)";
            return false;
        }
        const int hours = std::atoi(std::string{rest}.c_str());
        if (hours > 14) {
            err = "时区偏移超出范围 (最大 UTC+14:00)";
            return false;
        }
        out               = TimezoneSpec{};
        out.kind          = TimezoneSpec::Kind::FixedOffset;
        out.offsetMinutes = hours * 60;
        return true;
    }
    // 其余按 IANA 时区名 (`Asia/Shanghai`)
    if (!looksLikeZoneName(body)) {
        err = "时区写法不认识 (可用: auto / system / +08:00 / Asia/Shanghai)";
        return false;
    }
    if (timezoneNameSupported() && !zoneTzOffsetMinutesAt(body, systemNowMs()).has_value()) {
        err = "不认识的时区名 '" + std::string{body}
              + "' (示例: Asia/Shanghai; 想用固定偏移就写 +08:00)";
        return false;
    }
    out          = TimezoneSpec{};
    out.kind     = TimezoneSpec::Kind::Zone;
    out.zoneName = std::string{body};
    return true;
}

#if XX_IS_ANDROID_D || defined(_LIBCPP_VERSION) || XX_IS_MINGW_D
// Android NDK / libc++ (含 llvm-mingw) / MinGW 没有 chrono 时区数据库:
// 时区名解析不了, 只能按系统时区或固定偏移显示
bool timezoneNameSupported() noexcept {
    return false;
}

std::optional<int32_t> zoneTzOffsetMinutesAt(std::string_view zoneName, int64_t timestampMs) {
    (void)zoneName;
    (void)timestampMs;
    return std::nullopt;
}
#else
bool timezoneNameSupported() noexcept {
    return true;
}

std::optional<int32_t> zoneTzOffsetMinutesAt(std::string_view zoneName, int64_t timestampMs) {
    if (zoneName.empty() || timestampMs <= 0) {
        return std::nullopt;
    }
    try {
        const auto* zone = std::chrono::locate_zone(std::string{zoneName});
        if (zone == nullptr) {
            return std::nullopt;
        }
        const auto at   = std::chrono::sys_time<std::chrono::seconds>{
            std::chrono::seconds{static_cast<std::chrono::seconds::rep>(timestampMs / 1000)}
        };
        const auto info = zone->get_info(at);
        return static_cast<int32_t>(info.offset.count() / 60);
    } catch (const std::exception&) {
        // 名字无效或平台缺 tzdb: 当"解析不了", 由调用方退回系统时区
        return std::nullopt;
    }
}
#endif

int32_t systemTzOffsetMinutesAt(int64_t timestampMs) {
    const int64_t at = timestampMs > 0 ? timestampMs : systemNowMs();
    const std::time_t t = static_cast<std::time_t>(at / 1000);
    std::tm local{};
    std::tm utc{};
#if XX_IS_WIN_D
    if (localtime_s(&local, &t) != 0 || gmtime_s(&utc, &t) != 0) {
        return 0;
    }
#else
    if (localtime_r(&t, &local) == nullptr || gmtime_r(&t, &utc) == nullptr) {
        return 0;
    }
#endif
    // 两份字段都是同一个时间点: 差值就是本地时区相对 UTC 的偏移 (夏令时自然带上)
    const int64_t diffSec = secondsOfTm(local) - secondsOfTm(utc);
    return static_cast<int32_t>(diffSec / 60);
}

int32_t resolveTzOffsetMinutes(
    const TimezoneSpec& spec,
    int64_t             timestampMs,
    int32_t             eventOffsetMinutes
) {
    switch (spec.kind) {
        case TimezoneSpec::Kind::FixedOffset:
            return spec.offsetMinutes;
        case TimezoneSpec::Kind::Zone:
            if (const auto offset = zoneTzOffsetMinutesAt(spec.zoneName, timestampMs)) {
                return *offset;
            }
            return systemTzOffsetMinutesAt(timestampMs);
        case TimezoneSpec::Kind::System:
            return systemTzOffsetMinutesAt(timestampMs);
        case TimezoneSpec::Kind::Auto:
        default:
            if (eventOffsetMinutes != kUnknownTzOffset) {
                return eventOffsetMinutes;
            }
            return systemTzOffsetMinutesAt(timestampMs);
    }
}

std::string formatTimeAt(int64_t timestampMs, int32_t offsetMinutes, std::string_view pattern) {
    if (timestampMs <= 0 || pattern.empty()) {
        return {};
    }
    const int64_t   shifted = timestampMs / 1000 + static_cast<int64_t>(offsetMinutes) * 60;
    const std::time_t t     = static_cast<std::time_t>(shifted);
    std::tm         tmBuf{};
#if XX_IS_WIN_D
    if (gmtime_s(&tmBuf, &t) != 0) {
        return {};
    }
#else
    if (gmtime_r(&t, &tmBuf) == nullptr) {
        return {};
    }
#endif
    const std::string fmt{pattern};
    char              buf[64];
    const size_t      len = std::strftime(buf, sizeof(buf), fmt.c_str(), &tmBuf);
    if (len == 0) {
        return {};
    }
    return std::string{buf, len};
}

std::string formatTimeInZone(
    const TimezoneSpec& spec,
    int64_t             timestampMs,
    int32_t             eventOffsetMinutes,
    std::string_view    pattern
) {
    if (timestampMs <= 0) {
        return {};
    }
    const int32_t offset = resolveTzOffsetMinutes(spec, timestampMs, eventOffsetMinutes);
    return formatTimeAt(timestampMs, offset, pattern);
}

std::string tzOffsetLabel(int32_t offsetMinutes) {
    if (offsetMinutes == 0) {
        return "UTC";
    }
    const bool    negative = offsetMinutes < 0;
    const int32_t abs      = negative ? -offsetMinutes : offsetMinutes;
    char          buf[24];
    std::snprintf(
        buf,
        sizeof(buf),
        "UTC%s%02d:%02d",
        negative ? "-" : "+",
        static_cast<int>(abs / 60),
        static_cast<int>(abs % 60)
    );
    return std::string{buf};
}

std::string timezoneLabel(const TimezoneSpec& spec, int64_t atMs) {
    const int64_t at = atMs > 0 ? atMs : systemNowMs();
    switch (spec.kind) {
        case TimezoneSpec::Kind::FixedOffset:
            return tzOffsetLabel(spec.offsetMinutes);
        case TimezoneSpec::Kind::Zone:
            if (const auto offset = zoneTzOffsetMinutesAt(spec.zoneName, at)) {
                return spec.zoneName + " " + tzOffsetLabel(*offset);
            }
            return "系统本地 " + tzOffsetLabel(systemTzOffsetMinutesAt(at));
        case TimezoneSpec::Kind::System:
            return "系统本地 " + tzOffsetLabel(systemTzOffsetMinutesAt(at));
        case TimezoneSpec::Kind::Auto:
        default:
            return "自动 " + tzOffsetLabel(systemTzOffsetMinutesAt(at));
    }
}

} // namespace chatbot
} // namespace agentxx
