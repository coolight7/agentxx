///
/// 工具结果文本的失败前缀 (全项目唯一一份清单)
///
/// 背景: 工具结果是一段文本, "这次调用是否失败"除了执行层显式置位
/// (`tool_msg.extra["hasError"]`, 来自异常/工具不存在/参数非法/权限拒绝/插件报错)
/// 之外, 还允许工具**自己把失败写成结果文本**。为了不把每个工具的错误文本都改成
/// 结构化结果, 约定用固定前缀表示失败, 且判定只在
/// [toolcall.cpp](/agent/lib/src/nodes/toolcall.cpp) 一处做 (客户端不看前缀, 失败状态
/// 经 `ToolData::hasError` / `WireDelta::hasError` 下发)。
///
/// 因此: **任何作为工具结果返回的文本**, 首行都不能命中这里的前缀 (否则会被记成
/// 失败)。事件中枢的 `[Event ...]` / `[Event wait armed] ...` 等文本就是为了满足
/// 这条约束而选的前缀 (见 [events.md](/docs/zh-cn/design/events.md))。
///
/// 给"结果不是用户消息"的产物 (事件投递的输入等) 没有这条限制: 它们不是工具结果,
/// 不会经过失败判定。
#pragma once

#include <string_view>

namespace agentxx {
namespace util {

/// 工具结果文本表示失败的前缀 (按上界数量声明, 供 `std::span` 方式遍历)
inline constexpr std::string_view kToolFailurePrefixes[] = {
    "[Error",                  // 主机侧异常/参数错误 (节点拼出), 插件工具内部错误
    "[Permission denied",      // 权限拒绝
    "[Repeated call denied",   // 重复调用被用户拒绝
    "[Plugin tool timeout",    // 插件工具超时
};

/// 结果文本是否表示失败 (唯一判定实现)
/// - `args`:
///     - [text] 工具结果文本 (只看开头, 大小写敏感)
/// - `return` 首部命中失败前缀时为 true
inline bool toolResultTextIsFailure(std::string_view text) noexcept {
    for (const auto prefix : kToolFailurePrefixes) {
        if (text.starts_with(prefix)) {
            return true;
        }
    }
    return false;
}

} // namespace util
} // namespace agentxx
