#include "agentxx-client/io/tui/markdown_block.h"

#include "agentxx-client/io/tui/text_layout.h"
#include "markdown/parser.hpp"
#include <algorithm>
#include <string>

using namespace ftxui;

namespace agentxx {
namespace client {

namespace {
using agentxx::client::estimateLines;
} // namespace

/// 渲染 markdown 为 ftxui Element; 其中 ```mermaid 代码块由 DomBuilder 渲染为
/// 状态图 (见 markdown::build_code_block), 其余按 markdown 主题渲染
std::pair<Element, std::unique_ptr<markdown::DomBuilder>> renderMarkdown(
    std::string_view       content,
    Color                  color,
    markdown::Theme const& mdTheme,
    int                    maxWidth
) {
    if (content.empty()) {
        return {ftxui::text(""), nullptr};
    }
    auto parser  = markdown::make_cmark_parser();
    auto ast     = parser->parse(content);
    auto builder = std::make_unique<markdown::DomBuilder>();
    if (maxWidth > 0) {
        builder->set_max_width(maxWidth);
    }
    auto el = builder->build(ast, -1, mdTheme);
    return {el | ftxui::color(color), std::move(builder)};
}

/// 折叠消息头部单行预览的可用列数预算 (自适应宽度核心):

/// 估算 markdown 渲染高度 (行), 与 renderMarkdown (cmark-gfm + DomBuilder)
/// 的渲染语义对齐 (仅用于未进入视口的消息; 进入视口后实测修正)。
///
/// 背景: estimateLines 把每个 \n 都当硬换行, 但 DomBuilder 对普通段落把段内
/// 单个换行 (cmark softbreak) 合并为空格, 段落只按宽度折行 (仅空行分隔的
/// 段落间插入 1 行空行)。对"多行短句"文本 (LLM 输出常见, 如每行一个要点
/// 但无空行分隔), 按 \n 计数会严重高估 —— 例: 80 行 × 40 字符在 97 列下
/// 估算 80 行, 实际合并折行仅 ~33 行。不可见项高估即总高度虚高 ->
/// stickToBottom 滚动偏移偏大, 顶部消息被推出视口显示空白, 且不可见项
/// 永不进入视口实测 -> 空白持续 (用户报告"上半几条消息不渲染/可用高度
/// 变小")。
///
/// 估算规则 (近似, 尽量不高估):
/// - 普通段落: 段内换行折叠为单个空格, 再按宽度折行 (与
///   build_wrapping_container 的 ftxui::paragraph 合并语义一致)
/// - 行首标记行 (标题 # / 引用 > / 列表 - * + 数字. / 表格 |): 每源行渲染
///   1+ 行, 按去除标记后内容宽度折行估算 (build_list_item/blockquote 等
///   均为每源行一行, 内容处再按段落折行)
/// - ``` / ~~~ 围栏代码块: 开始围栏 1 行 + 内容行 + 结束围栏 1 行; 其中
///   超出可用宽度的代码行会被折成多行, 行数用与渲染侧同一个函数
///   [markdown::wrap_line_by_width] 按同一可用宽度算出 (build_code_block
///   折行语义), 否则长行代码块的高度会被低估
/// - ```mermaid 围栏: 渲染为状态图 (节点框 + 箭头), 图形高度与源行数无关,
///   实测约为源行数 × 3 + 3 (4 节点 5 边 TB 图: 7 源行 -> 24 行)。若按
///   普通代码块估算 (每行 1 行), 严重低估 (7 -> 8), 视口外消息总高度偏低,
///   滚动偏移偏小, 底部内容被推出视口且该 mermaid 消息被 continue 跳过
///   永不实测 -> 视口内显示空白 (用户报告"某些消息显示为空白, 滑动到
///   某些位置又正常")。故按 源行数 × 3 + 3 估算, 残余偏差由
///   LazyScrollable 的可见性容错 (kEstimateSlack) 提前实测自愈
/// - 块级元素间空行: 与 build_document 的 vbox({text(""), ...}) 一致,
///   第 2 个块起每块前 +1 行
size_t estimateMarkdownLines(std::string_view s, int width) {
    if (s.empty()) {
        return 1;
    }
    const size_t useWidth       = (width <= 0) ? 80 : static_cast<size_t>(width);
    // 代码行折行可用列数: 与 build_code_block 一致 (总宽度 - 左右各 1 列内边距,
    // <= 0 表示不折行); 外层缩进 (块引用/列表) 未计入, 该情形下估算略低
    const int    codeAvail      = static_cast<int>(useWidth) - 2;
    size_t       total          = 0;
    size_t       blocks         = 0; // 渲染块计数 (块间空行 +1, build_document 语义)
    bool         inFence        = false;
    bool         fenceIsMermaid = false; // 当前围栏是否为 ```mermaid (图形估算)
    size_t       fenceLines     = 0;     // 当前 mermaid 围栏源行数 (含开始/结束围栏)

    std::string para; // 普通段落累积 (softbreak -> 空格合并)
    auto        flushParagraph = [&]() {
        if (para.empty()) {
            return;
        }
        total += estimateLines(para, static_cast<int>(useWidth));
        para.clear();
        ++blocks;
    };

    /// 围栏信息串是否为 mermaid (大小写不敏感, 容忍首尾空白) —— 与
    /// dom_builder 的 is_mermaid_fence 语义一致
    auto isMermaidInfo = [](std::string_view info) {
        size_t b = info.find_first_not_of(" \t");
        size_t e = info.find_last_not_of(" \t");
        if (b == std::string_view::npos) {
            return false;
        }
        info = info.substr(b, e - b + 1);
        return info.size() >= 7 && info.substr(0, 7) == "mermaid";
    };

    const size_t n = s.size();
    size_t       i = 0;
    while (i < n) {
        const size_t     eol     = s.find('\n', i);
        const size_t     lineEnd = (eol == std::string_view::npos) ? n : eol;
        const std::string_view rawLine = s.substr(i, lineEnd - i); // 未去空白 (折行按原文算)
        std::string_view       line    = rawLine;
        const size_t           b       = line.find_first_not_of(" \t");
        const size_t           e       = line.find_last_not_of(" \t");
        line = (b == std::string_view::npos) ? std::string_view{} : line.substr(b, e - b + 1);
        if (line.empty()) {
            // 空行: 段落终止 (围栏内空行属于代码内容, 渲染 1 行)
            if (inFence) {
                ++total;
                if (fenceIsMermaid) {
                    ++fenceLines;
                }
            } else {
                flushParagraph();
            }
            i = (eol == std::string_view::npos) ? n : eol + 1;
            continue;
        }
        if (inFence) {
            if (fenceIsMermaid) {
                // 图形高度与源行数无关 (见上方说明), 先按源行计数再统一补足
                ++total;
                ++fenceLines;
            } else {
                // 代码行: 超宽时按可用列数折行 (与渲染侧同一折行函数)
                total += markdown::wrap_line_by_width(rawLine, codeAvail).size();
            }
            if (line.size() >= 3 && (line.substr(0, 3) == "```" || line.substr(0, 3) == "~~~")) {
                inFence = false; // 结束围栏 (已计 1 行)
                if (fenceIsMermaid) {
                    // 图形高度估算: 源行数 × 3 + 3 (实测 4 节点 5 边 TB 图
                    // 7 源行 = 24 行); 已按普通行计 fenceLines 行, 补足差额
                    total += fenceLines * 2 + 3;
                }
                fenceIsMermaid = false;
                fenceLines     = 0;
            }
            i = (eol == std::string_view::npos) ? n : eol + 1;
            continue;
        }
        const bool isFenceStart
            = line.size() >= 3 && (line.substr(0, 3) == "```" || line.substr(0, 3) == "~~~");
        if (isFenceStart) {
            flushParagraph();
            ++total; // 开始围栏 1 行
            ++blocks;
            inFence        = true;
            fenceIsMermaid = isMermaidInfo(line.substr(3));
            fenceLines     = 1;
            i              = (eol == std::string_view::npos) ? n : eol + 1;
            continue;
        }
        // 块级标记行 (标题/引用/列表/分隔线/表格): 每源行渲染 1+ 行
        const char c0           = line[0];
        const bool isMarkerLine = (c0 == '#') || (c0 == '>') || (c0 == '-') || (c0 == '*')
                                  || (c0 == '+') || (c0 == '|') || (c0 == '=');
        const bool isOrderedList
            = (line.size() >= 2 && c0 >= '0' && c0 <= '9' && (line[1] == '.' || line[1] == ')'));
        if (isMarkerLine || isOrderedList) {
            flushParagraph();
            // 去除行首标记序列后按内容折行估算 (渲染时内容宽度更窄, 已偏低估)
            const size_t cs       = line.find_first_not_of("#>-*+|= .");
            const auto   content  = (cs == std::string_view::npos || cs >= line.size())
                                        ? std::string_view{}
                                        : line.substr(cs);
            total                += estimateLines(content, static_cast<int>(useWidth));
            ++blocks;
            i = (eol == std::string_view::npos) ? n : eol + 1;
            continue;
        }
        // 普通文本行: 并入段落 (softbreak 合并, 行间以单个空格连接)
        if (!para.empty()) {
            para += ' ';
        }
        para += line;
        i     = (eol == std::string_view::npos) ? n : eol + 1;
    }
    flushParagraph();
    // 块间空行 (build_document: 第 2 个块起每块前 1 行空行)
    if (blocks > 1) {
        total += blocks - 1;
    }
    return std::max<size_t>(1, total);
}

} // namespace client
} // namespace agentxx
