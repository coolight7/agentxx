#include "agentxx-client/io/tui/mermaid_state.h"

#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include <markdown/text_utils.hpp>
#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace ftxui;

namespace agentxx {
namespace client {
namespace tui {
namespace {

// ---------------------------------------------------------------------------
// 文本小工具
// ---------------------------------------------------------------------------

std::string_view trimSv(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

bool startsWithSv(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

bool iequalsSv(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i]))
            != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> splitLines(std::string_view text) {
    std::vector<std::string> out;
    size_t                   start = 0;
    while (start <= text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) {
            out.emplace_back(text.substr(start));
            break;
        }
        out.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

/// 按显示宽度截断字符串 (宽字符按 2 列计), 过长时以 "…" 结尾
std::string truncateToWidth(std::string_view s, int maxWidth) {
    if (maxWidth <= 0) {
        return std::string(s);
    }
    if (markdown::utf8_display_width(s) <= maxWidth) {
        return std::string(s);
    }
    if (maxWidth == 1) {
        return "\xE2\x80\xA6"; // "…"
    }
    auto        bytePos = markdown::visual_col_to_byte(s, maxWidth - 1);
    std::string out(s.substr(0, bytePos));
    out += "\xE2\x80\xA6";
    return out;
}

/// 右侧补空格到指定显示宽度
std::string padToWidth(std::string_view s, int width) {
    std::string out(s);
    int         cur = markdown::utf8_display_width(out);
    while (cur < width) {
        out += ' ';
        ++cur;
    }
    return out;
}

// ---------------------------------------------------------------------------
// 连接方向位 (用于转角/分叉/汇合字形选择): N=上 S=下 E=右 W=左
// ---------------------------------------------------------------------------

constexpr int kN = 1;
constexpr int kS = 2;
constexpr int kE = 4;
constexpr int kW = 8;

/// TB 布局转角/分叉/汇合统一表 (连接方向集 → 字形)
/// 同列既可能是源侧 (茎自上而下, N) 又可能是目标侧 (向下进盒, S),
/// 取并集后查表, 避免源/目标转角互相覆盖
std::string glyphForTbJunction(int dirs) {
    switch (dirs) {
        case kN | kS:            return "\xE2\x94\x82"; // │ 直连
        case kN | kE:            return "\xE2\x94\x94"; // └ 向下转右
        case kN | kW:            return "\xE2\x94\x98"; // ┘ 向下转左
        case kN | kE | kW:       return "\xE2\x94\xB4"; // ┴ 分叉 (自上而下分裂左右)
        case kN | kS | kE:       return "\xE2\x94\x9C"; // ├ 直连 + 右分支
        case kN | kS | kW:       return "\xE2\x94\xA4"; // ┤ 直连 + 左分支
        case kN | kS | kE | kW:  return "\xE2\x94\xBC"; // ┼ 直连 + 双向分支
        case kE | kS:            return "\xE2\x94\x8C"; // ┌ 自右转下
        case kW | kS:            return "\xE2\x94\x90"; // ┐ 自左转下
        case kE | kW | kS:       return "\xE2\x94\xAC"; // ┬ 汇合 (左右汇合向下)
        case kE | kW:            return "\xE2\x94\x80"; // ─ (防御)
        default:                 return " ";
    }
}

/// LR 布局转角/分叉/汇合统一表 (连接方向集 → 字形)
/// 同行既可能是源侧 (茎自左向右, W) 又可能是目标侧 (向右进盒, E)
std::string glyphForLrJunction(int dirs) {
    switch (dirs) {
        case kW | kE:            return "\xE2\x94\x80"; // ─ 直连
        case kW | kN:            return "\xE2\x94\x98"; // ┘ 自左转上
        case kW | kS:            return "\xE2\x94\x90"; // ┐ 自左转下
        case kW | kE | kN:       return "\xE2\x94\xB4"; // ┴ 直连 + 上分支
        case kW | kE | kS:       return "\xE2\x94\xAC"; // ┬ 直连 + 下分支
        case kW | kN | kS:       return "\xE2\x94\xA4"; // ┤ 分叉 (自左分裂上下)
        case kW | kE | kN | kS:  return "\xE2\x94\xBC"; // ┼
        case kE | kN:            return "\xE2\x94\x94"; // └ 自上转右
        case kE | kS:            return "\xE2\x94\x8C"; // ┌ 自下转右
        case kE | kN | kS:       return "\xE2\x94\x9C"; // ├ 汇合 (上下汇合向右)
        case kN | kS:            return "\xE2\x94\x82"; // │ (防御)
        default:                 return " ";
    }
}

// ---------------------------------------------------------------------------
// 字符网格: 统一着色的文本片段按 (行, 列) 摆放
//
// 约定: 向量下标 == 网格列。多字符文本单元 (如整行盒子文本) 占据 width 列,
// 其后的 width-1 个槽位标记为"续列" (渲染时跳过), 保证后续单元写入的列号
// 与显示位置一致。
// ---------------------------------------------------------------------------

struct Cell {
    std::string text;        // 空 + 非续列 = 空格
    int         width = 0;   // 文本单元占用列数 (仅 text 非空有意义)
    bool        colored = false;
    Color       color;
    bool        continuation = false; // 前一多列单元的续列占位
};

class Grid {
public:

    explicit Grid(Color defaultColor) :
        defaultColor_(defaultColor) {}

    int rows() const {
        return static_cast<int>(cells_.size());
    }

    /// 写入一段文本; Color::Default 表示不着色 (继承外层装饰)
    void putStyled(int row, int col, std::string_view text, Color color) {
        put(row, col, text, color != Color::Default, color);
    }

    Element render() const {
        Elements rows;
        rows.reserve(cells_.size());
        for (const auto& row : cells_) {
            rows.push_back(renderRow(row));
        }
        return vbox(std::move(rows));
    }

private:

    void put(int row, int col, std::string_view text, bool colored, Color color) {
        if (text.empty()) {
            return;
        }
        ensure(row, col);
        int w = markdown::utf8_display_width(text);
        if (w <= 0) {
            w = 1;
        }
        ensure(row, col + w - 1);

        bool allSpace = true;
        for (char c : text) {
            if (c != ' ') {
                allSpace = false;
                break;
            }
        }
        if (allSpace) {
            // 空格只填空位, 不覆盖已有内容
            const auto& slot = cells_[row][col];
            if (slot.text.empty() && !slot.continuation) {
                cells_[row][col] = Cell{};
            }
            return;
        }
        cells_[row][col] = Cell{std::string(text), w, colored, color, false};
        // 续列占位: 使向量下标 == 显示列 (宽字符/整行文本)
        for (int i = 1; i < w; ++i) {
            cells_[row][col + i] = Cell{"", 0, colored, color, true};
        }
    }

    void ensure(int row, int col) {
        if (row >= static_cast<int>(cells_.size())) {
            cells_.resize(row + 1);
        }
        if (col >= static_cast<int>(cells_[row].size())) {
            cells_[row].resize(col + 1);
        }
        cols_ = std::max(cols_, col + 1);
    }

    /// 将一行渲染为 hbox: 相邻同色文本片段合并为一个 text 元素
    Element renderRow(const std::vector<Cell>& row) const {
        struct Run {
            std::string text;
            bool        colored;
            Color       color;
        };
        std::vector<Run> runs;
        runs.reserve(row.size());
        size_t i = 0;
        while (i < row.size()) {
            const auto& c = row[i];
            if (c.continuation) {
                ++i; // 多列单元的续列: 跳过 (文本已在首槽)
                continue;
            }
            if (c.text.empty()) {
                // 连续空格合并为一个 run
                size_t j = i;
                while (j < row.size() && row[j].text.empty() && !row[j].continuation) {
                    ++j;
                }
                runs.push_back(Run{std::string(j - i, ' '), false, Color::Default});
                i = j;
                continue;
            }
            // 相邻同色文本单元合并 (不跨空格)
            size_t   j = i;
            std::string text;
            while (j < row.size() && !row[j].text.empty() && !row[j].continuation
                   && row[j].colored == c.colored && row[j].color == c.color
                   && text.size() < 4096) {
                text += row[j].text;
                ++j;
            }
            runs.push_back(Run{std::move(text), c.colored, c.color});
            i = j;
        }
        Elements els;
        els.reserve(runs.size());
        for (const auto& r : runs) {
            if (r.colored && r.color != Color::Default) {
                els.push_back(text(r.text) | color(r.color));
            } else {
                els.push_back(text(r.text));
            }
        }
        return hbox(std::move(els));
    }

    std::vector<std::vector<Cell>> cells_;
    int                            cols_ = 0;
    Color                          defaultColor_;
};

// ---------------------------------------------------------------------------
// 布局结构
// ---------------------------------------------------------------------------

struct BoxGeom {
    std::vector<std::string> lines; // 盒子各行文本
    int                      width  = 0; // 盒宽 (终端列)
    int                      height = 0; // 盒高 (行)
};

constexpr int kBoxGap = 2; // 同层盒子间距 (TB: 列; LR: 行)

const char* kHorizBorder = "\xE2\x94\x80"; // ─ 水平边框
const char* kVertBorder  = "\xE2\x94\x82"; // │ 垂直边框

std::string repeatStr(std::string_view s, int n) {
    std::string out;
    out.reserve(s.size() * static_cast<size_t>(std::max(n, 0)));
    for (int i = 0; i < n; ++i) {
        out += s;
    }
    return out;
}

/// 构建节点盒子 (含边框); maxInterior <= 0 不截断
BoxGeom buildBox(const MermaidStateNode& node, int maxInterior) {
    std::vector<std::string> labelLines;
    if (node.isPseudo) {
        labelLines.push_back("[*]");
    } else {
        auto raw = node.label.empty() ? node.id : node.label;
        for (auto& l : splitLines(raw)) {
            std::string line = l;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            labelLines.push_back(maxInterior > 0 ? truncateToWidth(line, maxInterior)
                                                 : std::move(line));
        }
        if (labelLines.empty()) {
            labelLines.push_back("");
        }
    }
    if (node.isPseudo) {
        // 伪状态: 单行 [*], 无边框
        return BoxGeom{std::move(labelLines), 3, 1};
    }
    int interior = 0;
    for (const auto& l : labelLines) {
        interior = std::max(interior, markdown::utf8_display_width(l));
    }
    std::string horiz = repeatStr(kHorizBorder, interior);
    std::vector<std::string> lines;
    lines.push_back("\xE2\x94\x8C" + horiz + "\xE2\x94\x90"); // ┌ ┐
    for (const auto& l : labelLines) {
        lines.push_back(std::string(kVertBorder) + padToWidth(l, interior) + kVertBorder);
    }
    lines.push_back("\xE2\x94\x94" + horiz + "\xE2\x94\x98"); // └ ┘
    // 注意: 须先取 height 再 std::move(lines), 否则 lines.size() 读到的是
    // 被移动后的空 vector
    const int h = static_cast<int>(lines.size());
    return BoxGeom{std::move(lines), interior + 2, h};
}

/// 分层结果: 层内按节点注册顺序排列
struct Layout {
    std::vector<std::vector<size_t>> layers; // 按层分组
    std::vector<int>                 layerOf; // 节点 -> 层号
    int                              maxLayer = 0;
};

/// 分层: 最长路径法 (自源向汇)
///
/// layer[u] = 从任意源到 u 的最长路径长度。直接正向计算而非
/// "maxDist - distToSink" 取反 —— 取反只对位于全局最长路径上的节点正确,
/// 短分支 (如 in_progress → failed 快速失败路径) 会被错误地推到最大层。
/// 环通过递归访问标记截断 (回边进入图例)。
Layout computeLayers(const MermaidStateDiagram& dg) {
    const size_t                n = dg.nodes.size();
    std::vector<int>            layer(n, -1);
    std::vector<int8_t>         visiting(n, 0);
    std::function<int(size_t)>  longestFromSource = [&](size_t u) -> int {
        if (layer[u] >= 0) {
            return layer[u];
        }
        if (visiting[u]) {
            return 0; // 环: 回边不计
        }
        visiting[u] = 1;
        int best = 0;
        for (const auto& e : dg.edges) {
            if (e.to != u) {
                continue;
            }
            best = std::max(best, longestFromSource(e.from) + 1);
        }
        visiting[u] = 0;
        layer[u] = best;
        return best;
    };
    for (size_t i = 0; i < n; ++i) {
        longestFromSource(i);
    }

    Layout out;
    out.layerOf.assign(n, 0);
    int maxLayer = 0;
    for (size_t i = 0; i < n; ++i) {
        out.layerOf[i] = layer[i];
        maxLayer       = std::max(maxLayer, layer[i]);
    }
    out.maxLayer = maxLayer;
    out.layers.assign(maxLayer + 1, {});
    for (size_t i = 0; i < n; ++i) {
        out.layers[out.layerOf[i]].push_back(i);
    }
    return out;
}

struct BandEdge {
    size_t      src, dst;
    std::string label;
};

struct OtherEdge {
    std::string from, to, label;
};

/// 边分类: 相邻层边进边带, 其余 (跨层/回边/自环) 进图例
void classifyEdges(
    const MermaidStateDiagram&              dg,
    const Layout&                           layout,
    std::vector<std::vector<BandEdge>>&     bands,
    std::vector<OtherEdge>&                 others
) {
    bands.assign(layout.maxLayer + 1, {});
    for (const auto& e : dg.edges) {
        int lf = layout.layerOf[e.from];
        int lt = layout.layerOf[e.to];
        if (lt == lf + 1) {
            bands[lf].push_back(BandEdge{e.from, e.to, e.label});
            continue;
        }
        auto firstLine = [](const std::string& s) -> std::string {
            auto p = s.find('\n');
            return p == std::string::npos ? s : s.substr(0, p);
        };
        const auto& fn = dg.nodes[e.from];
        const auto& tn = dg.nodes[e.to];
        others.push_back(OtherEdge{
            firstLine(fn.label.empty() ? fn.id : fn.label),
            firstLine(tn.label.empty() ? tn.id : tn.label),
            e.label,
        });
    }
}

// ---------------------------------------------------------------------------
// 边带路由 (TB)
// ---------------------------------------------------------------------------

void drawBandTB(
    Grid&                        grid,
    const std::vector<BandEdge>& edges,
    const std::vector<int>&      centers,
    int                          row,
    Color                        defaultColor
) {
    // row: 带起始行 (盒子底下一行); 3 行: 茎 / 连接 / 箭头
    std::map<int, std::vector<const BandEdge*>> bySrc, byDst;
    for (const auto& e : edges) {
        bySrc[centers[e.src]].push_back(&e);
        byDst[centers[e.dst]].push_back(&e);
    }
    // 茎
    for (const auto& [col, es] : bySrc) {
        grid.putStyled(row, col, "│", defaultColor);
    }
    // 横向连接
    for (const auto& e : edges) {
        int cs = centers[e.src];
        int cd = centers[e.dst];
        if (cs == cd) {
            continue;
        }
        int lo = std::min(cs, cd);
        int hi = std::max(cs, cd);
        for (int x = lo + 1; x < hi; ++x) {
            grid.putStyled(row + 1, x, "─", defaultColor);
        }
    }
    // 转角/分叉/汇合: 同列合并源侧与目标侧连接方向后统一查表
    std::map<int, int> junctionDirs;
    for (const auto& [col, es] : bySrc) {
        int dirs = kN;
        for (const auto* e : es) {
            int cd = centers[e->dst];
            if (cd < col) {
                dirs |= kW;
            } else if (cd > col) {
                dirs |= kE;
            } else {
                dirs |= kS;
            }
        }
        junctionDirs[col] |= dirs;
    }
    for (const auto& [col, es] : byDst) {
        int dirs = kS;
        for (const auto* e : es) {
            int cs = centers[e->src];
            if (cs < col) {
                dirs |= kW;
            } else if (cs > col) {
                dirs |= kE;
            } else {
                dirs |= kN;
            }
        }
        junctionDirs[col] |= dirs;
    }
    for (const auto& [col, dirs] : junctionDirs) {
        grid.putStyled(row + 1, col, glyphForTbJunction(dirs), defaultColor);
    }
    // 箭头
    for (const auto& [col, es] : byDst) {
        grid.putStyled(row + 2, col, "v", defaultColor);
    }
    // 边标签 (箭头右侧)
    for (const auto& e : edges) {
        if (e.label.empty()) {
            continue;
        }
        grid.putStyled(row + 2, centers[e.dst] + 2, e.label, defaultColor);
    }
}

// ---------------------------------------------------------------------------
// 边带路由 (LR)
// ---------------------------------------------------------------------------

void drawBandLR(
    Grid&                        grid,
    const std::vector<BandEdge>& edges,
    const std::vector<BoxGeom>&  geom,
    const std::vector<int>&      tops,
    const std::vector<int>&      lefts,
    int                          bandCol,
    Color                        defaultColor
) {
    // bandCol: 带起始列 (本层最右列 + 1); 3 列: 茎 / 连接 / 箭头 (+ 标签列)
    auto axisOf = [&](size_t u) {
        return tops[u] + (geom[u].height - 1) / 2;
    };
    std::map<int, std::vector<const BandEdge*>> bySrc, byDst;
    for (const auto& e : edges) {
        bySrc[axisOf(e.src)].push_back(&e);
        byDst[axisOf(e.dst)].push_back(&e);
    }
    // 茎: 源行从盒子右缘到带起点列
    for (const auto& [ax, es] : bySrc) {
        int right = lefts[es.front()->src] + geom[es.front()->src].width;
        for (int x = right; x <= bandCol; ++x) {
            grid.putStyled(ax, x, "─", defaultColor);
        }
    }
    // 连接列竖向
    for (const auto& e : edges) {
        int sa = axisOf(e.src);
        int da = axisOf(e.dst);
        if (sa == da) {
            continue;
        }
        int lo = std::min(sa, da);
        int hi = std::max(sa, da);
        for (int y = lo + 1; y < hi; ++y) {
            grid.putStyled(y, bandCol + 1, "│", defaultColor);
        }
    }
    // 转角/分叉/汇合: 同行合并源侧与目标侧连接方向后统一查表
    std::map<int, int> junctionDirs;
    for (const auto& [ax, es] : bySrc) {
        int dirs = kW;
        for (const auto* e : es) {
            int da = axisOf(e->dst);
            if (da < ax) {
                dirs |= kN;
            } else if (da > ax) {
                dirs |= kS;
            } else {
                dirs |= kE;
            }
        }
        junctionDirs[ax] |= dirs;
    }
    for (const auto& [ax, es] : byDst) {
        int dirs = kE;
        for (const auto* e : es) {
            int sa = axisOf(e->src);
            if (sa < ax) {
                dirs |= kN;
            } else if (sa > ax) {
                dirs |= kS;
            } else {
                dirs |= kW;
            }
        }
        junctionDirs[ax] |= dirs;
    }
    for (const auto& [ax, dirs] : junctionDirs) {
        grid.putStyled(ax, bandCol + 1, glyphForLrJunction(dirs), defaultColor);
    }
    // 箭头
    for (const auto& [ax, es] : byDst) {
        grid.putStyled(ax, bandCol + 2, ">", defaultColor);
    }
    // 边标签 (箭头右侧, 占据本带标签列)
    for (const auto& e : edges) {
        if (e.label.empty()) {
            continue;
        }
        grid.putStyled(axisOf(e.dst), bandCol + 3, e.label, defaultColor);
    }
}

// ---------------------------------------------------------------------------
// 实际渲染
// ---------------------------------------------------------------------------

Color nodeDisplayColor(
    const MermaidStateDiagram&                          dg,
    const std::function<Color(std::string_view)>&       nodeColor,
    Color                                               defaultColor,
    size_t                                              idx
) {
    Color c = nodeColor ? nodeColor(dg.nodes[idx].id) : Color::Default;
    if (c == Color::Default) {
        c = defaultColor;
    }
    return c;
}

/// 将图例 (跨层/回边/自环边) 追加为文本行
void renderLegend(
    Grid&                      grid,
    const std::vector<OtherEdge>& others,
    Color                      defaultColor
) {
    int row = grid.rows() + 1;
    for (const auto& e : others) {
        std::string line = e.from + " --> " + e.to;
        if (!e.label.empty()) {
            line += ": " + e.label;
        }
        grid.putStyled(row++, 0, line, defaultColor);
    }
}

Element renderTB(
    const Layout&                                        layout,
    const std::vector<BoxGeom>&                          geom,
    const std::vector<std::vector<BandEdge>>&            bands,
    const std::vector<OtherEdge>&                        others,
    const MermaidStateDiagram&                           dg,
    const std::function<Color(std::string_view)>&        nodeColor,
    Color                                                defaultColor
) {
    Grid grid(defaultColor);
    const size_t        n = dg.nodes.size();
    std::vector<int>    lefts(n, 0), centers(n, 0);
    // Pass 1: 计算所有层盒子的位置与中心列 (边带路由需目标中心, 须先于绘制)
    for (int L = 0; L <= layout.maxLayer; ++L) {
        const auto& layer = layout.layers[L];
        int         x     = 0;
        for (size_t u : layer) {
            lefts[u]   = x;
            centers[u] = x + (geom[u].width - 1) / 2;
            x += geom[u].width + kBoxGap;
        }
    }
    // Pass 2: 放置盒子 + 绘制边带
    int row = 0;
    for (int L = 0; L <= layout.maxLayer; ++L) {
        const auto& layer = layout.layers[L];
        int         layerH = 0;
        for (size_t u : layer) {
            layerH = std::max(layerH, geom[u].height);
        }
        for (size_t u : layer) {
            int  top = row + (layerH - geom[u].height) / 2; // 垂直居中
            auto c   = nodeDisplayColor(dg, nodeColor, defaultColor, u);
            for (int i = 0; i < geom[u].height; ++i) {
                grid.putStyled(top + i, lefts[u], geom[u].lines[i], c);
            }
        }
        row += layerH;
        if (L < layout.maxLayer) {
            if (!bands[L].empty()) {
                drawBandTB(grid, bands[L], centers, row, defaultColor);
                row += 3;
            } else {
                row += 1; // 无连接的层间隙
            }
        }
    }
    if (!others.empty()) {
        renderLegend(grid, others, defaultColor);
    }
    return grid.render();
}

Element renderLR(
    const Layout&                                        layout,
    const std::vector<BoxGeom>&                          geom,
    const std::vector<std::vector<BandEdge>>&            bands,
    const std::vector<OtherEdge>&                        others,
    const MermaidStateDiagram&                           dg,
    const std::function<Color(std::string_view)>&        nodeColor,
    Color                                                defaultColor
) {
    Grid grid(defaultColor);
    const size_t        n = dg.nodes.size();
    std::vector<int>    tops(n, 0), lefts(n, 0);
    // 每带标签列宽 (影响后续层起点)
    std::vector<int>    bandLabelW(layout.maxLayer + 1, 0);
    for (int L = 0; L <= layout.maxLayer; ++L) {
        if (L < layout.maxLayer) {
            for (const auto& e : bands[L]) {
                bandLabelW[L] = std::max(bandLabelW[L], markdown::utf8_display_width(e.label));
            }
        }
    }
    // Pass 1: 布局 (top/left 供边带路由, 须先于绘制)
    {
        int col = 0;
        for (int L = 0; L <= layout.maxLayer; ++L) {
            const auto& layer = layout.layers[L];
            int         layerW = 0;
            for (size_t u : layer) {
                layerW = std::max(layerW, geom[u].width);
            }
            int top = 0;
            for (size_t u : layer) {
                tops[u]  = top;
                lefts[u] = col + (layerW - geom[u].width) / 2; // 水平居中
                top += geom[u].height + kBoxGap;
            }
            col += layerW;
            if (L < layout.maxLayer) {
                col += bands[L].empty() ? 1 : (3 + bandLabelW[L]);
            }
        }
    }
    // Pass 2: 放置盒子 + 绘制边带
    int col = 0;
    for (int L = 0; L <= layout.maxLayer; ++L) {
        const auto& layer = layout.layers[L];
        int         layerW = 0;
        for (size_t u : layer) {
            layerW = std::max(layerW, geom[u].width);
        }
        for (size_t u : layer) {
            auto c = nodeDisplayColor(dg, nodeColor, defaultColor, u);
            for (int i = 0; i < geom[u].height; ++i) {
                grid.putStyled(tops[u] + i, lefts[u], geom[u].lines[i], c);
            }
        }
        col += layerW;
        if (L < layout.maxLayer) {
            if (!bands[L].empty()) {
                drawBandLR(grid, bands[L], geom, tops, lefts, col, defaultColor);
                col += 3 + bandLabelW[L];
            } else {
                col += 1;
            }
        }
    }
    if (!others.empty()) {
        renderLegend(grid, others, defaultColor);
    }
    return grid.render();
}

} // namespace

// ---------------------------------------------------------------------------
// 解析
// ---------------------------------------------------------------------------

MermaidStateDiagram parseMermaidStateDiagram(std::string_view source) {
    MermaidStateDiagram dg;
    std::map<std::string, size_t, std::less<>> idIndex;
    // 起始/结束伪状态使用互不相同的内部 id (显示均为 [*])
    const std::string kStartId = "\x01start\x01";
    const std::string kEndId   = "\x01end\x01";

    auto ensureNode = [&](std::string id, bool pseudo, std::string label) -> size_t {
        auto it = idIndex.find(id);
        if (it != idIndex.end()) {
            // 节点已存在 (可能由边引用先行创建): 补充标签声明
            // (state "label" as id 出现在转移之后时, 标签不能丢失)
            auto& node = dg.nodes[it->second];
            if (!label.empty()) {
                node.label = std::move(label);
            }
            return it->second;
        }
        size_t idx = dg.nodes.size();
        dg.nodes.push_back(MermaidStateNode{std::move(id), std::move(label), pseudo});
        idIndex.emplace(dg.nodes.back().id, idx);
        return idx;
    };

    int compositeDepth = 0; // 复合状态体深度 (其内容不支持, 忽略)
    for (const auto& rawLine : splitLines(source)) {
        std::string_view line = trimSv(rawLine);
        if (line.empty()) {
            continue;
        }
        if (line == "}") {
            if (compositeDepth > 0) {
                --compositeDepth;
            }
            continue;
        }
        if (compositeDepth > 0) {
            continue; // 复合状态体内容 (嵌套 state/转移等) 忽略
        }
        if (startsWithSv(line, "%%")) {
            continue;
        }
        if (iequalsSv(line, "stateDiagram") || iequalsSv(line, "stateDiagram-v2")) {
            continue;
        }
        if (startsWithSv(line, "direction")) {
            auto dir = trimSv(line.substr(9));
            if (iequalsSv(dir, "LR") || iequalsSv(dir, "RL")) {
                dg.directionLR = true;
            }
            continue;
        }
        if (startsWithSv(line, "state ")) {
            auto        rest = trimSv(line.substr(6));
            std::string id, label;
            if (!rest.empty() && rest.front() == '"') {
                auto close = rest.find('"', 1);
                if (close == std::string_view::npos) {
                    continue;
                }
                label = std::string(trimSv(rest.substr(1, close - 1)));
                auto  tail = trimSv(rest.substr(close + 1));
                if (startsWithSv(tail, "as")) {
                    id = std::string(trimSv(tail.substr(2)));
                } else {
                    id = label; // 无 as: id 即标签
                }
            } else {
                id = std::string(rest);
            }
            bool openComposite = false;
            if (!id.empty() && id.back() == '{') {
                id.pop_back();
                openComposite = true;
            }
            id = std::string(trimSv(id));
            if (!id.empty()) {
                ensureNode(std::move(id), false, std::move(label));
                if (openComposite) {
                    ++compositeDepth;
                }
            }
            continue;
        }
        auto arrowPos = line.find("-->");
        if (arrowPos != std::string_view::npos) {
            auto             from = trimSv(line.substr(0, arrowPos));
            auto             tail = trimSv(line.substr(arrowPos + 3));
            std::string_view to   = tail;
            std::string_view label;
            auto             colon = tail.find(':');
            if (colon != std::string_view::npos) {
                to    = trimSv(tail.substr(0, colon));
                label = trimSv(tail.substr(colon + 1));
            }
            if (from.empty() || to.empty()) {
                continue;
            }
            size_t fi, ti;
            if (from == "[*]") {
                fi = ensureNode(kStartId, true, "[*]");
            } else {
                fi = ensureNode(std::string(from), false, {});
            }
            if (to == "[*]") {
                ti = ensureNode(kEndId, true, "[*]");
            } else {
                ti = ensureNode(std::string(to), false, {});
            }
            dg.edges.push_back(MermaidStateEdge{fi, ti, std::string(label)});
            continue;
        }
        // 其余 (note/复合状态体/未知语法): 忽略
    }
    return dg;
}

// ---------------------------------------------------------------------------
// 渲染入口
// ---------------------------------------------------------------------------

Element renderMermaidStateDiagram(
    const MermaidStateDiagram&                          dg,
    int                                                 maxWidth,
    Color                                               defaultColor,
    const std::function<Color(std::string_view)>&       nodeColor
) {
    if (dg.nodes.empty()) {
        return text("");
    }

    auto layout = computeLayers(dg);
    const size_t n = dg.nodes.size();
    std::vector<BoxGeom> geom(n);
    for (size_t i = 0; i < n; ++i) {
        geom[i] = buildBox(dg.nodes[i], 0);
    }

    std::vector<std::vector<BandEdge>> bands;
    std::vector<OtherEdge>             others;
    classifyEdges(dg, layout, bands, others);

    // 宽度适配: 按层行宽预算截断标签 (TB 约束; LR 顺带受益)
    if (maxWidth > 0) {
        for (const auto& layer : layout.layers) {
            int rowW = 0;
            for (size_t u : layer) {
                rowW += geom[u].width;
            }
            rowW += kBoxGap * (static_cast<int>(layer.size()) - 1);
            if (rowW <= maxWidth) {
                continue;
            }
            int num    = static_cast<int>(layer.size());
            int budget = (maxWidth - 4 * num + 2) / num; // 边框 2n + 间隙 2(n-1)
            if (budget < 1) {
                budget = 1;
            }
            for (size_t u : layer) {
                if (dg.nodes[u].isPseudo) {
                    continue;
                }
                BoxGeom b = buildBox(dg.nodes[u], budget);
                if (b.width < geom[u].width) {
                    geom[u] = std::move(b);
                }
            }
        }
    }

    // LR 宽度超限时退回 TB (TB 可按层截断)
    bool preferLR = dg.directionLR;
    if (preferLR && maxWidth > 0) {
        int lrWidth = 0;
        for (int L = 0; L <= layout.maxLayer; ++L) {
            int layerW = 0;
            for (size_t u : layout.layers[L]) {
                layerW = std::max(layerW, geom[u].width);
            }
            lrWidth += layerW;
            if (L < layout.maxLayer) {
                if (bands[L].empty()) {
                    lrWidth += 1;
                } else {
                    int lbl = 0;
                    for (const auto& e : bands[L]) {
                        lbl = std::max(lbl, markdown::utf8_display_width(e.label));
                    }
                    lrWidth += 3 + lbl;
                }
            }
        }
        if (lrWidth > maxWidth) {
            preferLR = false;
        }
    }

    if (preferLR) {
        return renderLR(layout, geom, bands, others, dg, nodeColor, defaultColor);
    }
    return renderTB(layout, geom, bands, others, dg, nodeColor, defaultColor);
}

} // namespace tui
} // namespace client
} // namespace agentxx
