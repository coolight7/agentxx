#pragma once

/// markdown 块渲染 (消息正文/思考内容与中断 markdown 块共用)
///
/// 从 message_list.cpp 抽出为共享实现: 中断描述块的 markdown 内容与
/// Assistant/Think 消息正文使用同一套渲染与行数估算, 避免两处漂移。
#include "ftxui/dom/elements.hpp"
#include "markdown/dom_builder.hpp"
#include "markdown/text_utils.hpp"
#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>

namespace agentxx {
namespace client {

/// 渲染 markdown 为 ftxui Element; 其中 ```mermaid 代码块由 DomBuilder 渲染为
/// 状态图 (见 markdown::build_code_block), 其余按 markdown 主题渲染
/// - `return` {元素, DomBuilder}; DomBuilder 须与元素同生命周期 (元素内部
///   容器/链接 Box 指向它), 由调用方持有
std::pair<ftxui::Element, std::unique_ptr<markdown::DomBuilder>> renderMarkdown(
    std::string_view       content,
    ftxui::Color           color,
    markdown::Theme const& mdTheme,
    int                    maxWidth = 0
);

/// 估算 markdown 渲染高度 (行), 与 renderMarkdown (cmark-gfm + DomBuilder)
/// 的渲染语义对齐 (仅用于未进入视口的消息; 进入视口后实测修正)
size_t estimateMarkdownLines(std::string_view s, int width);

} // namespace client
} // namespace agentxx
