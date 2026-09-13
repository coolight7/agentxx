#pragma once

#include <string>
#include <string_view>
#include <vector>

/// 文本布局辅助 (消息列表 / 中断视图 / 折叠预览共用; 显示宽度口径统一为
/// markdown::utf8_display_width: 宽字符 (CJK/emoji) 按 2 列计)
namespace agentxx {
namespace client {

/// 估算文本显示行数 (换行符计数 + 按显示宽度折行估算)
/// - 仅用于不可见子项的高度估算 (影响滚动条/滚动定位), 进入视口后实测修正
/// - 线性扫描, 不整串调用 utf8_display_width (避免 O(n²))
size_t estimateLines(std::string_view s, int width);

/// 将单行或多行文本按可用显示宽度折行为多段字符串
/// - 支持按 \n 换行, 同时对超宽单行 (如无空格的长文件路径/URL) 按 UTF-8
///   字符显示宽度硬折行
///
/// - `args`:
///     - [textContent] 源文本
///     - [maxWidth] 单行最大可用显示列宽, 应当 >= 1
///
/// - `return` 折行后的字符串行列表 (源文本为空时返回空列表)
std::vector<std::string> wrapTextToLines(std::string_view textContent, int maxWidth);

/// 折叠消息头部单行预览的可用列数预算 (自适应宽度核心):
/// 内容区总列数 - 头部前缀显示列数 - 安全余量1列 (防边界取整溢出);
/// 极窄终端下保底 8 列, 避免预览被完全挤没
int collapsedPreviewBudget(int maxWidth, int prefixCols);

/// 取最后一个非空行 (供"末尾思考"折叠预览使用)
std::string_view lastNonBlankLine(std::string_view s);

} // namespace client
} // namespace agentxx
