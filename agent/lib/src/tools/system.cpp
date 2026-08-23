#include "agentxx/tools/system.h"

#include "fmt/format.h"
#include <chrono>
#include <ctime>
#include <format>
#include <sstream>
#include <string>

namespace agentxx {
namespace tools {

GetCurrentDateTimeTool::GetCurrentDateTimeTool(
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    XXToolBase("agentxx_get_current_datetime", in_agentContext, false, true) {}

neograph::ChatTool GetCurrentDateTimeTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    // 无参数工具也声明空对象 schema: parameters 为 null 会被部分严格网关
    // (如 SCNet) 拒绝, 返回 400 "Format Error"
    return {
        get_name(),
        prompt.depict,
        neograph::json{
                       {"type", "object"},
                       {"properties", neograph::json::object()},
                       },
    };
}

asio::awaitable<std::string> GetCurrentDateTimeTool::execute_async(const neograph::json& arguments
) {
    auto now = std::chrono::system_clock::now();

    // 本地时间: 优先使用 tzdb (chrono::current_zone), 无 tzdata 环境 (如精简 Android)
    // 会抛异常, 降级为 C 库 localtime 计算
    // NOTE: Android NDK libc++ 未实现 chrono tzdb (current_zone/zoned_time 不存在),
    // 属于编译期缺失而非运行时异常, 必须条件编译直接走 localtime 路径
    std::string localTimeStr;
#if XX_IS_ANDROID_D
    {
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm     tmv{};
        localtime_r(&t, &tmv);
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

    co_return fmt::format(
        R"(Timestamp: {} millisecond
Local Time (24Hour): {}
UTC Time (24Hour): {})",
        // 用 duration_cast 保证跨平台正确 (system_clock 周期在 Windows/Android 上非纳秒)
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count(),
        localTimeStr,
        std::format("{:%Y-%m-%d %H:%M:%S}", now)
    );
}

}; // namespace tools
}; // namespace agentxx
