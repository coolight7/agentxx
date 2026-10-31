#pragma once

/// 时间与时区: 把平台时间戳解析成时间点, 再按配置的时区显示给人和模型
///
/// 这里有两件事, 必须分开处理:
/// - **时间点**: 平台时间戳 (`2026-10-29T12:03:09+08:00`) 里的时区后缀要真的用上。
///   以前是"只取前 19 个字符, 后缀一律当 UTC", 于是同一个时间点被算成比真实值早
///   8 小时; 显示时再按本地时区转一次, 消息就显示成 8 小时后 —— 模型据此谈时间,
///   和用户所在地区对不上(问"现在几点"时最容易看出来)。
/// - **显示时区**: 时间点转成给人看的钟点时用哪个时区。由 `behavior.timezone` 决定,
///   不再各调各的 `localtime`。
///
/// `behavior.timezone` 的写法:
/// - `auto` (默认): 用平台消息时间戳自带的偏移; 平台没给时退回系统时区;
/// - `system` / `local`: 系统时区 (跟随运行环境, 夏令时按该时间点算);
/// - `+08:00` / `-05:00` / `+0800` / `UTC+8` / `8`: 固定偏移;
/// - `Asia/Shanghai`: IANA 时区名 (运行平台没有时区库时退回系统时区并记警告)。
///
/// 本头不引用任何主程序内部头 (插件边界), 只用标准库。
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace agentxx {
namespace chatbot {

/// 平台没提供时区时的偏移占位值 (分钟)
///
/// 用 `INT32_MIN` 而不是 0: 0 是合法偏移(UTC), 两者必须能区分开。
inline constexpr int32_t kUnknownTzOffset = INT32_MIN;

/// 系统时钟的当前毫秒 (epoch; 与消息时间戳同一时钟)
int64_t systemNowMs();

/// RFC3339 / ISO8601 时间戳的解析结果
struct Rfc3339Time {
    /// epoch 毫秒 (解析失败为 0)
    int64_t ms = 0;
    /// 时间戳自带的 UTC 偏移 (分钟; 东区为正); 没带时区 = [kUnknownTzOffset]
    int32_t offsetMinutes = kUnknownTzOffset;

    bool ok() const noexcept {
        return ms > 0;
    }
};

/// 解析 RFC3339 / ISO8601 形态的时间戳
///
/// - 接受 `2026-10-29T12:03:09+08:00` / `...Z` / `...+0800` / `...+08` / 带小数秒
/// - 分隔符 `T` 与空格都认; 尾部多余的空白忽略
/// - 完全没带时区时按 `defaultOffsetMinutes` 解释 (默认 0 = UTC)
/// - 年月日时分秒越界一律当解析失败 (返回的对象 `ok()` 为 false)
///
/// - `args`:
///     - [text] 平台给的时间串
///     - [defaultOffsetMinutes] 没有时区后缀时用的偏移 (分钟)
Rfc3339Time parseRfc3339Time(std::string_view text, int32_t defaultOffsetMinutes = 0);

/// 时区的写法 (见文件头的四种)
struct TimezoneSpec {
    enum class Kind : uint8_t {
        /// 跟随平台消息时间戳, 没有时退回系统时区
        Auto = 0,
        /// 系统时区
        System = 1,
        /// 固定偏移
        FixedOffset = 2,
        /// IANA 时区名
        Zone = 3,
    };

    Kind kind = Kind::Auto;
    /// 固定偏移 (分钟; 东区为正)
    int32_t offsetMinutes = 0;
    /// IANA 时区名 (仅 `Kind::Zone`)
    std::string zoneName;
};

/// 解析 `behavior.timezone` 的写法
///
/// - 空串按 `auto` 处理
/// - 失败时不修改 `out`, 用 `err` 说明原因 (可直接进配置错误信息)
bool parseTimezoneSpec(std::string_view text, TimezoneSpec& out, std::string& err);

/// 运行平台的 chrono 时区库是否可用
///
/// - Android / libc++ / MinGW 没有 tzdb: 时区名不可解析, 只能按系统时区或固定偏移
bool timezoneNameSupported() noexcept;

/// 某个时间点的系统时区偏移 (分钟; 夏令时按该时间点算)
int32_t systemTzOffsetMinutesAt(int64_t timestampMs);

/// 某个时间点的 IANA 时区偏移 (分钟)
///
/// - `return` 平台不支持时区库 / 名字无效 / 时间点非法时为空
std::optional<int32_t> zoneTzOffsetMinutesAt(std::string_view zoneName, int64_t timestampMs);

/// 决定一次显示用哪个偏移 (分钟)
///
/// - `System` / `FixedOffset` / `Zone` 按配置来 (`Zone` 取不到时退回系统时区)
/// - `Auto` 优先用 `eventOffsetMinutes` (平台自带的), 没有时退回系统时区
///
/// - `args`:
///     - [spec] 配置的时区
///     - [timestampMs] 要显示的时间点
///     - [eventOffsetMinutes] 消息时间戳自带的偏移 ([kUnknownTzOffset] = 没有)
int32_t resolveTzOffsetMinutes(
    const TimezoneSpec& spec,
    int64_t             timestampMs,
    int32_t             eventOffsetMinutes
);

/// 按给定偏移格式化时间点
///
/// 实现上把时间点平移偏移量后按 UTC 拆字段, 因此不依赖进程的 `TZ`
/// (服务器跑在 UTC、而用户在东八区时不会互相干扰)。
///
/// - `args`:
///     - [timestampMs] epoch 毫秒 (<= 0 返回空串)
///     - [offsetMinutes] UTC 偏移 (分钟)
///     - [pattern] `strftime` 格式串
std::string formatTimeAt(int64_t timestampMs, int32_t offsetMinutes, std::string_view pattern);

/// 按配置的时区格式化时间点 (显示时间的统一入口)
std::string formatTimeInZone(
    const TimezoneSpec& spec,
    int64_t             timestampMs,
    int32_t             eventOffsetMinutes,
    std::string_view    pattern
);

/// 偏移的展示名 (`UTC+08:00` / `UTC` / `UTC-05:30`)
std::string tzOffsetLabel(int32_t offsetMinutes);

/// 时区的展示名 (给模型与日志看, 让它知道现在用的是哪个时区)
///
/// - 固定偏移: `UTC+08:00`
/// - 系统时区: `系统本地 UTC+08:00`
/// - `auto`: `自动 UTC+08:00` (跟随平台消息, 当前环境是东八区)
/// - 时区名: `Asia/Shanghai UTC+08:00` (平台不支持时区库时退回系统时区)
///
/// - `args`:
///     - [spec] 配置的时区
///     - [atMs] 取哪个时间点的偏移显示 (夏令时会变)
std::string timezoneLabel(const TimezoneSpec& spec, int64_t atMs);

} // namespace chatbot
} // namespace agentxx
