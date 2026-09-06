/// agentxx_system 插件 —— 工具实现 (纯函数, 不含 C ABI 胶水)
/// - 从 libagentxx src/tools/system 拆分: agentxx_get_current_datetime 同名同行为
/// - 头文件-only: 插件入口与测试共同包含, 保证插件行为与测试覆盖一致
#pragma once

#include <chrono>
#include <ctime>
#include <fmt/format.h>
#include <format>
#include <string>

namespace agentxx_system_plugin {

/// 获取当前时间描述文本 (原 GetCurrentDateTimeTool::execute_async)
/// - 输出三行: Timestamp 毫秒 / 本地 24 小时制 / UTC 24 小时制
inline std::string currentDatetimeExecute() {
    auto now = std::chrono::system_clock::now();

    // 本地时间: 优先使用 tzdb (chrono::current_zone), 无 tzdata 环境 (如精简 Android)
    // 会抛异常, 降级为 C 库 localtime 计算
    // NOTE: Android NDK / llvm-mingw libc++ 未实现 chrono tzdb (current_zone/zoned_time 不存在),
    // 属于编译期缺失而非运行时异常, 必须条件编译直接走 localtime 路径
    std::string localTimeStr;
#if XX_IS_ANDROID_D || defined(_LIBCPP_VERSION) || defined(__MINGW32__) \
    || (XX_IS_WIN_D && !defined(_MSC_VER))
    {
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm     tmv{};
#if XX_IS_WIN_D
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        localTimeStr = fmt::format(
            "{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}",
            tmv.tm_year + 1900,
            tmv.tm_mon + 1,
            tmv.tm_mday,
            tmv.tm_hour,
            tmv.tm_min,
            tmv.tm_sec
        );
    }
#else
    try {
        std::chrono::zoned_time local_time{std::chrono::current_zone(), now};
        localTimeStr = std::format("{:%Y-%m-%d %H:%M:%S}", local_time);
    } catch (...) {
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm     tmv{};
#if XX_IS_WIN_D
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        localTimeStr = fmt::format(
            "{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}",
            tmv.tm_year + 1900,
            tmv.tm_mon + 1,
            tmv.tm_mday,
            tmv.tm_hour,
            tmv.tm_min,
            tmv.tm_sec
        );
    }
#endif

    return fmt::format(
        R"(Timestamp: {} millisecond
Local Time (24Hour): {}
UTC Time (24Hour): {})",
        // 用 duration_cast 保证跨平台正确 (system_clock 周期在 Windows/Android 上非纳秒)
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count(),
        localTimeStr,
        std::format("{:%Y-%m-%d %H:%M:%S}", now)
    );
}

} // namespace agentxx_system_plugin
