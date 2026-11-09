#pragma once

#include <string_view>

namespace agentxx {
namespace events {

/// 投递文本的段落标记 (`renderEventText` 生成, 展示端也按它分段)
///
/// - [kEventContentSection]: 本次投递有值, 内容跟在标记行之后
/// - [kEventContentEmptySection]: 本次投递没有值 (例如流结束了但一个值都没有),
///   单独一行, 后面不再补占位文本 —— 与 `agentxx_execute_command` 的空段写法
///   (`[StdOut: empty]` / `[StdErr: empty]`) 保持同一种样式, 一眼看出"这一段没有东西"
///
/// 放在独立头里是为了让展示端 (TUI) 不必为一个常量包含整个事件中枢头。
inline constexpr std::string_view kEventContentSection      = "[Content]";
inline constexpr std::string_view kEventContentEmptySection = "[Content: empty]";

} // namespace events
} // namespace agentxx
