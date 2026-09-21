#pragma once

/// 终端显示列宽计算 (宽字符按 2 列, 组合字符按 0 列)
///
/// 背景: 表格/计量条/迷你趋势图等新组件需要"按显示列宽"排版与截断, 而项目里
/// 原有多套口径 (markdown 渲染库的宽度函数、FTXUI 的 `string_width`、行数估算里
/// 的近似算法)。本文件提供一个不依赖任何 UI 库的实现, 放在 libagentxx 内,
/// 宿主、测试与插件都可直接使用, 新组件一律以它为准。
///
/// 已有两套口径保持不变 (不要求一次性替换, 避免大面积回归):
/// - markdown 渲染继续使用 markdown-ui 的 `utf8_display_width`
/// - FTXUI 元素自身的测量继续由 FTXUI 完成
#include <cstdint>
#include <string>
#include <string_view>

namespace agentxx {
namespace ui {

/// 单个码点的显示列宽 (不参与排版的码点返回 0)
/// - `args`:
///     - [codePoint] Unicode 码点
/// - `return` 组合字符/零宽字符返回 0, 东亚宽字符与 emoji 返回 2, 其余返回 1
int codePointWidth(char32_t codePoint);

/// 解码下一个 UTF-8 码点
/// - 非法字节序列按 U+FFFD 处理并只前进 1 字节 (保证调用方总能推进)
/// - `args`:
///     - [text] 源文本
///     - [pos] 传入起始字节下标; 返回时指向下一码点的起始位置
/// - `return` 解码出的码点
char32_t decodeNextCodePoint(std::string_view text, size_t& pos);

/// 文本的显示列宽 (不含换行符之后的宽度计算, 整串累计)
/// - `args`:
///     - [text] UTF-8 文本 (可含换行, 换行符本身按 1 列计, 调用方自行按行拆分)
int displayWidth(std::string_view text);

/// 按显示列宽截断 (宽字符安全: 不会留下只占一半的宽字符)
/// - 未超宽时原样返回; 超宽时保留前若干列并追加省略号 (省略号自身宽度也计入)
/// - `args`:
///     - [text] 源文本
///     - [maxWidth] 允许的最大显示列宽 (<=0 返回空串)
///     - [ellipsis] 截断标记 (默认 "…"; 传空串表示直接截断不追加标记)
std::string truncateToWidth(std::string_view text, int maxWidth, std::string_view ellipsis = "…");

/// 按显示列宽在右侧补空格 (已超宽时原样返回, 由调用方自行截断)
std::string padRightToWidth(std::string_view text, int width);

/// 从文本头部取出不超过 [maxWidth] 列的完整前缀 (宽字符安全)
/// - `return` 实际占用的显示列宽 (<= maxWidth)
std::string_view prefixByWidth(std::string_view text, int maxWidth, int& usedWidth);

} // namespace ui
} // namespace agentxx
