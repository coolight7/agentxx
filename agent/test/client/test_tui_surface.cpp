// TUI 弹窗  面性风格测试 (离屏渲染)
//
// 覆盖场景:
// - [区域背景色] 标题栏 / 内容区 / 底部提示栏分别使用主题的
//   surfaceHeaderColor / surfaceColor / surfaceFooterColor
//   (错误类弹窗标题栏改用 surfaceErrorHeaderColor)
// - [角标与外框] 四角为 "+" 角标 (前景=内容区背景色, 背景=弹窗外部色), 且不含边框/
//   分割线字符
// - [外框留白] 上下左右各 2 格内边距 + 三区域之间各 1 行间距, 全部由外框提供:
//   标题/内容行都不再自带首尾留白 (文字直接贴外框留白左边界)
// - [区域顺序] 标题栏位于弹窗顶部, 底部提示栏位于弹窗底部, 之间为内容区
// - [整行高亮] 列表类弹窗选中项背景覆盖整行 (面性风格的选中表达)
// - [主题配色] Dark/Light 下三个区域背景色两两不同, 且区别于整体背景与蒙版色
// - [插件 items separator 项] 渲染为浅色背景区块而非横线
#include "agentxx-test/client/test_tui_surface.h"

#include "agentxx-client/io/tui/components/overlays.h"
#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/framework/tui_settings.h"
#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx-client/io/tui/surface.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "agentxx/util/json.h"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_tui_surface_passed = 0;
int g_tui_surface_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_tui_surface_passed
#define XX_TEST_FAILED g_tui_surface_failed

namespace agentxx {
namespace test {

using namespace agentxx::client;

namespace {

/// 边框字符集 (面性风格弹窗不应出现)
/// - 仅列**边框/角/交叉**字符与**水平分割线**字符 (含 ftxui border 的 │ ─ ┌ ┐ └ ┘)
/// - 不含滚动条 thumb 字符 (┃ ╹ ╻): 滚动条是交互控件而非分割线,
///   由 Scrollable::drawScrollbar / ftxui vscroll_indicator 绘制
/// - 不含插件/消息内容自身的图形字符 (表格/代码块等由内容决定)
const char* const kFrameGlyphs[] = {
    "─", "│", "┌", "┐", "└", "┘", "├", "┤", "┬", "┴", "┼", "═", "║", "╔",
    "╗", "╚", "╝", "╠", "╣", "╦", "╩", "╬", "╭", "╮", "╰", "╯", "━", "┏",
    "┓", "┗", "┛", "┣", "┫", "┳", "┻", "╋", "▁", "▔", "┄", "┅", "┈", "┉",
};

/// 剥离 ANSI CSI 转义序列, 仅保留可见字符 (与 test_tui_context_overlay 同款)
std::string stripAnsi(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size();) {
        if (raw[i] == '\x1B') {
            size_t j = i + 1;
            if (j < raw.size() && raw[j] == '[') {
                ++j;
                while (j < raw.size() && !isalpha(static_cast<unsigned char>(raw[j]))) {
                    ++j;
                }
                if (j < raw.size()) {
                    ++j;
                }
            } else if (j < raw.size()) {
                ++j;
            }
            i = j;
            continue;
        }
        out += raw[i++];
    }
    return out;
}

/// 弹窗离屏渲染结果 (屏幕 + 可见文本 + 弹窗区域包围盒)
struct ProbeResult {
    ftxui::Screen screen;
    std::string   text; ///< 剥离 ANSI 的可见文本 (整屏, 含弹窗外空白)
    /// 非默认背景色单元格的包围盒 = 弹窗实际占据区域 (空 Box = 未渲染出弹窗)
    ftxui::Box bounds{0, -1, 0, -1};

    ProbeResult(int w, int h) :
        screen(w, h) {}

    ftxui::Color bgAt(int x, int y) const {
        return screen.CellAt(x, y).background_color;
    }

    bool isEmpty() const {
        return bounds.IsEmpty();
    }

    int midX() const {
        return (bounds.x_min + bounds.x_max) / 2;
    }

    /// 在可见文本中定位 needle 首次出现的 (x, y); 未找到返回 false
    bool findText(std::string_view needle, int& outX, int& outY) const {
        int    y     = 0;
        size_t start = 0;
        while (start <= text.size()) {
            const size_t end = text.find('\n', start);
            const auto   line
                = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            const size_t pos = line.find(needle);
            if (pos != std::string::npos) {
                outX = static_cast<int>(pos);
                outY = y;
                return true;
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
            ++y;
        }
        return false;
    }
};

/// 渲染弹窗组件到固定尺寸屏幕
/// - 弹窗按自身逻辑决定尺寸 (未被弹窗覆盖的区域保持默认背景, 用于包围盒扫描)
ProbeResult renderProbe(ftxui::Component comp, int w, int h) {
    ProbeResult result(w, h);
    // 与 ModalContainer 一致: 弹窗按**自然尺寸**居中 (center = hcenter|vcenter,
    // 由 filler 吸收多余空间), 弹窗自身不随屏幕尺寸被拉伸
    auto el = comp->Render() | ftxui::center;
    ftxui::Render(result.screen, el);
    result.text = stripAnsi(result.screen.ToString());

    int xMin = w, xMax = -1, yMin = h, yMax = -1;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (result.bgAt(x, y) == ftxui::Color::Default) {
                continue;
            }
            xMin = std::min(xMin, x);
            xMax = std::max(xMax, x);
            yMin = std::min(yMin, y);
            yMax = std::max(yMax, y);
        }
    }
    if (xMax >= 0) {
        result.bounds = ftxui::Box{xMin, xMax, yMin, yMax};
    }
    return result;
}

/// 单元格是否为空白填充 (无可见字符; 内边距区域应为纯填充)
bool isBlankCell(const ProbeResult& r, int x, int y) {
    const auto& ch = r.screen.CellAt(x, y).character;
    return ch.empty() || ch == " ";
}

/// 四角角标检查: 角格为 "+" 角标, 前景=内容区背景色, 背景=弹窗外部色 (蒙版色)
void checkCornerCells(const ProbeResult& r, const TUITheme& theme, int line) {
    struct Corner {
        int x;
        int y;
    };

    const Corner corners[4] = {
        {r.bounds.x_min, r.bounds.y_min},
        {r.bounds.x_max, r.bounds.y_min},
        {r.bounds.x_min, r.bounds.y_max},
        {r.bounds.x_max, r.bounds.y_max},
    };
    for (const auto& c : corners) {
        const auto& cell = r.screen.CellAt(c.x, c.y);
        XX_TEST_EXPECT_EQ(cell.character, std::string("+"));
        XX_TEST_EXPECT_EQ(cell.foreground_color, theme.surfaceColor);
        XX_TEST_EXPECT_EQ(cell.background_color, theme.surfaceScrimColor);
    }
    (void)line;
}

/// 面性风格外框检查:
/// - 纵向: 上内边距 (角标行) / 标题栏 / 间距 / 内容区 / 间距 / 底部提示栏 / 下内边距 (角标行)
/// - 横向: 左右各 2 格内边距 (标题栏与底栏背景色不越界到内边距列)
void checkSurfaceRegions(
    const ProbeResult&  r,
    const TUITheme&     theme,
    const ftxui::Color& headerBg,
    int                 line,
    bool                headerTrailingControl = false
) {
    if (r.isEmpty()) {
        XX_TEST_FAILED++;
        TEST_FAIL << "line " << line << ": no popup rendered (empty surface bounds)" << std::endl;
        return;
    }
    XX_TEST_PASSED++; // 弹窗已渲染出面性区域

    const int midX = r.midX();
    // 上/下内边距 (角标行) 为内容区背景色; 标题栏/底栏各在其内侧一行, 与内容区之间各 1 行间距
    XX_TEST_EXPECT_EQ(r.bgAt(midX, r.bounds.y_min), theme.surfaceColor);
    XX_TEST_EXPECT_EQ(r.bgAt(midX, r.bounds.y_min + 1), headerBg);
    XX_TEST_EXPECT_EQ(r.bgAt(midX, r.bounds.y_min + 2), theme.surfaceColor);
    XX_TEST_EXPECT_EQ(r.bgAt(midX, r.bounds.y_max - 2), theme.surfaceColor);
    XX_TEST_EXPECT_EQ(r.bgAt(midX, r.bounds.y_max - 1), theme.surfaceFooterColor);
    XX_TEST_EXPECT_EQ(r.bgAt(midX, r.bounds.y_max), theme.surfaceColor);
    // 左右各 2 格内边距为内容区背景色 (区域背景不越界到内边距列)
    for (int dx = 0; dx <= 1; ++dx) {
        XX_TEST_EXPECT_EQ(r.bgAt(r.bounds.x_min + dx, r.bounds.y_min + 1), theme.surfaceColor);
        XX_TEST_EXPECT_EQ(r.bgAt(r.bounds.x_max - dx, r.bounds.y_min + 1), theme.surfaceColor);
        XX_TEST_EXPECT_EQ(r.bgAt(r.bounds.x_min + dx, r.bounds.y_max - 1), theme.surfaceColor);
        XX_TEST_EXPECT_EQ(r.bgAt(r.bounds.x_max - dx, r.bounds.y_max - 1), theme.surfaceColor);
    }
    // 区域背景铺满整行 (内容区宽度内的左端/右端同样为区域背景色), 即"整行色带"
    XX_TEST_EXPECT_EQ(r.bgAt(r.bounds.x_min + 2, r.bounds.y_min + 1), headerBg);
    if (!headerTrailingControl) { // 标题栏右端有控件时该列被控件背景占用
        XX_TEST_EXPECT_EQ(r.bgAt(r.bounds.x_max - 2, r.bounds.y_min + 1), headerBg);
    }
    XX_TEST_EXPECT_EQ(r.bgAt(r.bounds.x_min + 2, r.bounds.y_max - 1), theme.surfaceFooterColor);
    XX_TEST_EXPECT_EQ(r.bgAt(r.bounds.x_max - 2, r.bounds.y_max - 1), theme.surfaceFooterColor);
    // 四角角标
    checkCornerCells(r, theme, line);
    // 内边距为纯填充 (四角角标格除外): 上下内边距行与左右内边距列不含任何字符,
    // 即各区域文字/控件都被外框挡在内边距之内
    int padChars = 0;
    for (int x = r.bounds.x_min + 1; x < r.bounds.x_max; ++x) {
        if (!isBlankCell(r, x, r.bounds.y_min)) {
            ++padChars;
        }
        if (!isBlankCell(r, x, r.bounds.y_max)) {
            ++padChars;
        }
    }
    for (int y = r.bounds.y_min + 1; y < r.bounds.y_max; ++y) {
        if (!isBlankCell(r, r.bounds.x_min, y)) {
            ++padChars;
        }
        if (!isBlankCell(r, r.bounds.x_max, y)) {
            ++padChars;
        }
    }
    XX_TEST_EXPECT_EQ(padChars, 0);
    // 弹窗表面完整: 内部每一行都落在某个区域背景上 (无镂空/断口)
    int hollowRows = 0;
    for (int y = r.bounds.y_min; y <= r.bounds.y_max; ++y) {
        if (r.bgAt(midX, y) == ftxui::Color::Default) {
            ++hollowRows;
        }
    }
    XX_TEST_EXPECT_EQ(hollowRows, 0);
    // 区域间以背景色区分, 不需要边框: 三个区域背景两两不同
    XX_TEST_EXPECT_TRUE(headerBg != theme.surfaceColor);
    XX_TEST_EXPECT_TRUE(headerBg != theme.surfaceFooterColor);
    XX_TEST_EXPECT_TRUE(theme.surfaceColor != theme.surfaceFooterColor);
}

/// 弹窗内不应出现边框/分割线字符 (四角角标除外, 由 checkCornerCells 检查)
void checkNoFrameGlyphs(const ProbeResult& r, int line) {
    for (int y = r.bounds.y_min; y <= r.bounds.y_max; ++y) {
        for (int x = r.bounds.x_min; x <= r.bounds.x_max; ++x) {
            const bool isCorner = (x == r.bounds.x_min || x == r.bounds.x_max)
                                  && (y == r.bounds.y_min || y == r.bounds.y_max);
            if (isCorner) {
                continue;
            }
            const auto& ch = r.screen.CellAt(x, y).character;
            for (const char* glyph : kFrameGlyphs) {
                if (ch == glyph) {
                    XX_TEST_FAILED++;
                    TEST_FAIL << "line " << line << ": popup contains frame/divider glyph '"
                              << glyph << "' at (" << x << "," << y << ")" << std::endl;
                    return;
                }
            }
        }
    }
    XX_TEST_PASSED++;
}

/// 测试夹具: 最小 TUICtx (视图尺寸固定, 避免随真实终端漂移)
struct SurfaceFixture {
    TUISharedState sharedState;
    TUITheme       theme = TUITheme::darkTheme();

    TUICtx ctx;

    static constexpr int kScreenW = 100;
    static constexpr int kScreenH = 34;

    SurfaceFixture() {
        ctx.state          = &sharedState;
        ctx.frameState     = sharedState.readSnapshot();
        ctx.postRedraw     = [] {};
        ctx.theme          = &theme;
        ctx.sessionId      = "s1";
        ctx.remoteUrl      = "";
        ctx.viewportWidth  = 100;
        ctx.viewportHeight = 30;
    }

    /// 渲染探针: 渲染前刷新本帧状态快照 (与主渲染器每帧 readSnapshot 一致),
    /// 使 fixture 中 mutate 写入的状态对本次渲染可见
    ProbeResult probe(const ftxui::Component& comp) {
        ctx.frameState = sharedState.readSnapshot();
        return renderProbe(comp, kScreenW, kScreenH);
    }
};

} // namespace

TestResult testTuiSurface() {
    auto savedLang = TUISettings::instance().language();
    TUISettings::instance().setLanguage(TuiLanguage::ZhCn);

    // ---- 主题配色: 面性风格 token 齐备, 且各区域背景色可互相区分 (Dark/Light) ----
    for (const TUITheme& theme : {TUITheme::darkTheme(), TUITheme::lightTheme()}) {
        XX_TEST_EXPECT_TRUE(theme.surfaceColor != theme.surfaceHeaderColor); // 内容区/标题栏
        XX_TEST_EXPECT_TRUE(theme.surfaceColor != theme.surfaceFooterColor); // 内容区/底栏
        XX_TEST_EXPECT_TRUE(theme.surfaceHeaderColor != theme.surfaceFooterColor); // 标题栏/底栏
        XX_TEST_EXPECT_TRUE(
            theme.surfaceErrorHeaderColor != theme.surfaceHeaderColor
        ); // 错误标题栏
        // 弹窗/整体背景: 浅色主题下弹窗表面与整体背景同为白色, 由蒙版色衬托区分
        XX_TEST_EXPECT_TRUE(
            theme.surfaceColor != theme.backgroundColor
            || theme.surfaceScrimColor != theme.backgroundColor
        );
        XX_TEST_EXPECT_TRUE(theme.surfaceScrimColor != theme.surfaceColor); // 蒙版/弹窗
        XX_TEST_EXPECT_TRUE(theme.surfaceTitleColor != theme.surfaceColor); // 标题文字/内容区
    }

    // ---- 模型选择弹窗: 区域背景 + 选中项整行高亮 ----
    {
        SurfaceFixture fx;
        fx.sharedState.mutate([](TUIRenderState& st) {
            st.modelNames      = {"gpt-4o", "claude-3", "deepseek-v3"};
            st.cachedModelName = "claude-3";
            st.modelInfoLoaded = true;
        });
        auto comp = std::make_shared<ModelSelectorOverlay>(fx.ctx);
        auto r    = fx.probe(comp);

        checkSurfaceRegions(r, fx.theme, fx.theme.surfaceHeaderColor, __LINE__);
        checkNoFrameGlyphs(r, __LINE__);
        XX_TEST_EXPECT_TRUE(r.text.find("选择模型") != std::string::npos);

        // 标题与内容行都贴外框左内边距 (x_min + 2), 行内不再自带首尾空格
        int        tx = -1, ty = -1;
        const bool titleFound = r.findText("选择模型", tx, ty);
        XX_TEST_EXPECT_TRUE(titleFound);
        if (titleFound) {
            XX_TEST_EXPECT_EQ(tx, r.bounds.x_min + 2);
            XX_TEST_EXPECT_EQ(ty, r.bounds.y_min + 1); // 标题栏位于上内边距下一行
        }

        // 选中项 (cachedModelName 首帧对齐) 整行高亮: 行中与行尾皆为高亮背景色
        // —— 面性风格以整行色块表达选中; 列表行宽 = 内容区宽度 - 1 列滚动条
        // gutter, 故行尾为 x_max - 3 (内容区右端 x_max - 2 之内一列)
        int        sx = -1, sy = -1;
        const bool found = r.findText("claude-3", sx, sy);
        XX_TEST_EXPECT_TRUE(found);
        if (found) {
            XX_TEST_EXPECT_EQ(sx, r.bounds.x_min + 2);
            XX_TEST_EXPECT_EQ(r.bgAt(r.midX(), sy), fx.theme.buttonActiveBgColor);
            XX_TEST_EXPECT_EQ(r.bgAt(r.bounds.x_max - 3, sy), fx.theme.buttonActiveBgColor);
        }
        // 非选中项无高亮背景 (内容区背景色)
        int        ux = -1, uy = -1;
        const bool foundOthers = r.findText("gpt-4o", ux, uy);
        XX_TEST_EXPECT_TRUE(foundOthers);
        if (foundOthers) {
            XX_TEST_EXPECT_EQ(ux, r.bounds.x_min + 2);
            XX_TEST_EXPECT_EQ(r.bgAt(r.midX(), uy), fx.theme.surfaceColor);
        }
    }

    // ---- 会话选择弹窗 (列表 + 顶部新建入口) ----
    {
        SurfaceFixture fx;
        fx.sharedState.mutate([](TUIRenderState& st) {
            st.sessionListLoaded = true;
            st.sessionList       = {
                {"s1", "会话一", 1700000000000},
                {"s2", "会话二", 1700000001000},
            };
        });
        auto comp = std::make_shared<SessionSelectorOverlay>(fx.ctx);
        auto r    = fx.probe(comp);

        checkSurfaceRegions(r, fx.theme, fx.theme.surfaceHeaderColor, __LINE__);
        checkNoFrameGlyphs(r, __LINE__);
        XX_TEST_EXPECT_TRUE(r.text.find("选择会话") != std::string::npos);
        XX_TEST_EXPECT_TRUE(r.text.find("新会话") != std::string::npos);
        int        ix = -1, iy = -1;
        const bool itemFound = r.findText("会话一", ix, iy);
        XX_TEST_EXPECT_TRUE(itemFound);
        if (itemFound) {
            XX_TEST_EXPECT_EQ(ix, r.bounds.x_min + 2); // 条目贴外框左内边距
        }
    }

    // ---- 设置弹窗 ----
    {
        SurfaceFixture fx;
        auto           comp = std::make_shared<SettingsOverlay>(fx.ctx);
        auto           r    = fx.probe(comp);

        checkSurfaceRegions(r, fx.theme, fx.theme.surfaceHeaderColor, __LINE__);
        checkNoFrameGlyphs(r, __LINE__);
        XX_TEST_EXPECT_TRUE(r.text.find("设置") != std::string::npos);
        XX_TEST_EXPECT_TRUE(r.text.find("主题") != std::string::npos);
        XX_TEST_EXPECT_TRUE(r.text.find("动画等级") != std::string::npos);

        // 选中项 (第 0 项) 的"值行"整行高亮: 值文字起点 / 行中 / 行末 (右内边距之内)
        // 皆为高亮背景色 —— 高亮色带铺满整行, 而不是只覆盖值文字
        int        sx = -1, sy = -1;
        const bool selFound = r.findText("主题: ", sx, sy);
        XX_TEST_EXPECT_TRUE(selFound);
        if (selFound) {
            XX_TEST_EXPECT_EQ(r.bgAt(sx, sy), fx.theme.buttonActiveBgColor);
            XX_TEST_EXPECT_EQ(r.bgAt(r.midX(), sy), fx.theme.buttonActiveBgColor);
            XX_TEST_EXPECT_EQ(r.bgAt(r.bounds.x_max - 2, sy), fx.theme.buttonActiveBgColor);
        }

        // 非选中项 (第 1 项) 的值行: 同样为整行宽的浅色值色带 (值色为半透明按钮底色,
        // 与内容区背景混合后成色); 行首与行末同色, 不再只覆盖值文字本身
        int        ux = -1, uy = -1;
        const bool unselFound = r.findText("动画等级: ", ux, uy);
        XX_TEST_EXPECT_TRUE(unselFound);
        if (unselFound) {
            XX_TEST_EXPECT_TRUE(r.bgAt(ux, uy) != fx.theme.surfaceColor); // 值色带底色
            XX_TEST_EXPECT_EQ(r.bgAt(r.bounds.x_max - 2, uy), r.bgAt(ux, uy));
        }
    }

    // ---- Logs 侧边栏菜单弹窗 ----
    {
        SurfaceFixture fx;
        auto           comp = std::make_shared<LogMenuOverlay>(fx.ctx);
        auto           r    = renderProbe(comp, 40, 16);

        checkSurfaceRegions(r, fx.theme, fx.theme.surfaceHeaderColor, __LINE__);
        checkNoFrameGlyphs(r, __LINE__);
        XX_TEST_EXPECT_TRUE(r.text.find("菜单") != std::string::npos);
        XX_TEST_EXPECT_TRUE(r.text.find("LLM 上下文") != std::string::npos);

        // 默认选中项 (第 0 项) 整行高亮背景
        int        sx = -1, sy = -1;
        const bool found = r.findText("LLM 上下文", sx, sy);
        XX_TEST_EXPECT_TRUE(found);
        if (found) {
            XX_TEST_EXPECT_EQ(r.bgAt(r.midX(), sy), fx.theme.buttonActiveBgColor);
        }
    }

    // ---- 待发送消息队列弹窗 ----
    {
        SurfaceFixture fx;
        fx.sharedState.mutate([](TUIRenderState& st) {
            TUIPendingInput item;
            item.id   = "p1";
            item.text = "排队中的消息";
            st.pendingInputs.push_back(std::move(item));
        });
        auto comp = std::make_shared<PendingInputsOverlay>(fx.ctx);
        auto r    = fx.probe(comp);

        // 标题栏右端为"清空"按钮 (按钮背景色), 故不校验右端为标题栏背景色
        checkSurfaceRegions(r, fx.theme, fx.theme.surfaceHeaderColor, __LINE__, true);
        checkNoFrameGlyphs(r, __LINE__);
        XX_TEST_EXPECT_TRUE(r.text.find("待发送消息队列") != std::string::npos);
        XX_TEST_EXPECT_TRUE(r.text.find("排队中的消息") != std::string::npos);
    }

    // ---- 上下文弹窗 (LLM Context) ----
    {
        SurfaceFixture fx;
        fx.sharedState.mutate([](TUIRenderState& st) {
            st.contextMessages = std::make_shared<agentxx::util::Json>(agentxx::util::Json::parse(
                R"([{"role":"user","content":"你好"},{"role":"assistant","content":"在"}])"
            ));
        });
        auto comp = std::make_shared<ContextOverlay>(fx.ctx);
        auto r    = fx.probe(comp);

        checkSurfaceRegions(r, fx.theme, fx.theme.surfaceHeaderColor, __LINE__);
        checkNoFrameGlyphs(r, __LINE__);
        XX_TEST_EXPECT_TRUE(r.text.find("LLM 上下文") != std::string::npos);
        XX_TEST_EXPECT_TRUE(r.text.find("+ [user]") != std::string::npos);
    }

    // ---- 关于弹窗 (About) ----
    {
        SurfaceFixture fx;
        fx.ctx.dataDir = "/tmp/data";
        fx.ctx.workDir = "/tmp/work";
        auto comp      = std::make_shared<AboutOverlay>(fx.ctx);
        auto r         = fx.probe(comp);

        checkSurfaceRegions(r, fx.theme, fx.theme.surfaceHeaderColor, __LINE__);
        checkNoFrameGlyphs(r, __LINE__);
        XX_TEST_EXPECT_TRUE(r.text.find("关于") != std::string::npos);
        XX_TEST_EXPECT_TRUE(r.text.find("版本") != std::string::npos);
    }

    // ---- 通用 Text 弹窗 (open_overlay TEXT) ----
    {
        SurfaceFixture fx;
        auto comp = std::make_shared<TextOverlay>(fx.ctx, "自定义标题", "正文内容", false);
        auto r    = fx.probe(comp);

        checkSurfaceRegions(r, fx.theme, fx.theme.surfaceHeaderColor, __LINE__);
        checkNoFrameGlyphs(r, __LINE__);
        XX_TEST_EXPECT_TRUE(r.text.find("自定义标题") != std::string::npos);
        XX_TEST_EXPECT_TRUE(r.text.find("正文内容") != std::string::npos);
    }

    // ---- 通用 Custom 弹窗: 插件 items 的 separator 项渲染为浅色区块 (无横线) ----
    {
        SurfaceFixture fx;
        auto           items = agentxx::util::Json::parse(R"([
            {"kind": "text", "text": "第一项", "role": "normal"},
            {"kind": "separator"},
            {"kind": "badge", "text": "状态"}
        ])");
        auto comp = std::make_shared<CustomOverlay>(fx.ctx, "插件弹窗", std::move(items), "test");
        auto r = fx.probe(comp);

        checkSurfaceRegions(r, fx.theme, fx.theme.surfaceHeaderColor, __LINE__);
        checkNoFrameGlyphs(r, __LINE__);
        XX_TEST_EXPECT_TRUE(r.text.find("插件弹窗") != std::string::npos);
        XX_TEST_EXPECT_TRUE(r.text.find("第一项") != std::string::npos);

        // separator 项: 前一项下一行为浅色背景区块 (整行), 而不是 "─" 横线
        int        ix = -1, iy = -1;
        const bool found = r.findText("第一项", ix, iy);
        XX_TEST_EXPECT_TRUE(found);
        if (found) {
            XX_TEST_EXPECT_EQ(r.bgAt(r.midX(), iy + 1), fx.theme.surfaceFooterColor);
        }
    }

    // ---- 加载失败组件弹窗: 错误类弹窗标题栏用错误色背景 ----
    {
        SurfaceFixture fx;
        fx.sharedState.mutate([](TUIRenderState& st) {
            using Notif = agentxx::agent::AppendComponentNotification;
            st.appendComponents.push_back(
                Notif{Notif::Type::Plugin, "bad_plugin", false, "加载失败原因"}
            );
        });
        auto comp = std::make_shared<FailedComponentsOverlay>(fx.ctx);
        auto r    = fx.probe(comp);

        checkSurfaceRegions(r, fx.theme, fx.theme.surfaceErrorHeaderColor, __LINE__);
        checkNoFrameGlyphs(r, __LINE__);
        XX_TEST_EXPECT_TRUE(r.text.find("加载失败的组件") != std::string::npos);
        XX_TEST_EXPECT_TRUE(r.text.find("加载失败原因") != std::string::npos);
    }

    // ---- 多模态文件选择弹窗 (内容区 = 路径行 + 文件列表, 无过滤输入框) ----
    {
        SurfaceFixture                      fx;
        agentxx::agent::ModelCapabilityInfo caps;
        caps.name       = "test-model";
        caps.imageInput = true;
        auto comp       = std::make_shared<FilePickerOverlay>(fx.ctx, caps, "/tmp");
        auto r          = fx.probe(comp);

        checkSurfaceRegions(r, fx.theme, fx.theme.surfaceHeaderColor, __LINE__);
        checkNoFrameGlyphs(r, __LINE__);
        XX_TEST_EXPECT_TRUE(r.text.find("选择文件") != std::string::npos);
        XX_TEST_EXPECT_TRUE(r.text.find("/tmp") != std::string::npos);
        // 过滤输入框已移除: 弹窗内不再出现过滤标签
        XX_TEST_EXPECT_TRUE(r.text.find("过滤") == std::string::npos);
        XX_TEST_EXPECT_TRUE(r.text.find("Filter") == std::string::npos);
    }

    // ---- Mermaid 状态图弹窗 (外框为面性外框: 四角 + 角标; 状态图自身的框线在内容区, 不参与) ----
    {
        SurfaceFixture fx;
        auto           mermaid = std::string("stateDiagram-v2\n  [*] --> A\n  A --> B\n");
        auto           comp = std::make_shared<MermaidDiagramOverlay>(fx.ctx, mermaid, "状态图");
        auto           r    = fx.probe(comp);

        // checkSurfaceRegions 内含角标/内边距检查 (外框不含直线字符; 图内容自身框线不计)
        checkSurfaceRegions(r, fx.theme, fx.theme.surfaceHeaderColor, __LINE__);
        XX_TEST_EXPECT_TRUE(r.text.find("状态图") != std::string::npos);
        int gx = -1, gy = -1;
        XX_TEST_EXPECT_TRUE(r.findText("状态图", gx, gy));
        XX_TEST_EXPECT_EQ(gx, r.bounds.x_min + 2);
        XX_TEST_EXPECT_EQ(gy, r.bounds.y_min + 1);
    }

    // ---- Toast 提示 (仿照弹窗背景: 左右各 2 格留白, 四角为 '+') ----
    // 1. Dark 主题单行 toast
    {
        SurfaceFixture fx;
        const auto     style = TuiSurfaceStyle::fromTheme(fx.theme);
        const auto     toast = tuiSurfaceToast(style, "通知内容");

        ProbeResult r(fx.kScreenW, fx.kScreenH);
        auto        el = toast | ftxui::center;
        ftxui::Render(r.screen, el);
        r.text = stripAnsi(r.screen.ToString());

        int xMin = fx.kScreenW, xMax = -1, yMin = fx.kScreenH, yMax = -1;
        for (int y = 0; y < fx.kScreenH; ++y) {
            for (int x = 0; x < fx.kScreenW; ++x) {
                if (r.bgAt(x, y) == ftxui::Color::Default) {
                    continue;
                }
                xMin = std::min(xMin, x);
                xMax = std::max(xMax, x);
                yMin = std::min(yMin, y);
                yMax = std::max(yMax, y);
            }
        }
        XX_TEST_EXPECT_TRUE(xMax >= 0);
        r.bounds = ftxui::Box{xMin, xMax, yMin, yMax};

        // 四角角标: 字符为 '+', 前景=surfaceColor, 背景=surfaceScrimColor
        checkCornerCells(r, fx.theme, __LINE__);
        checkNoFrameGlyphs(r, __LINE__);

        // 高度为 3 行 (上内边距 1 行 + 文本 1 行 + 下内边距 1 行)
        XX_TEST_EXPECT_EQ(r.bounds.y_max - r.bounds.y_min + 1, 3);

        // 文本位于中间行 (y_min + 1), 左侧内边距正好 2 格 (x_min + 2)
        int tx = -1, ty = -1;
        XX_TEST_EXPECT_TRUE(r.findText("通知内容", tx, ty));
        XX_TEST_EXPECT_EQ(ty, r.bounds.y_min + 1);
        XX_TEST_EXPECT_EQ(tx, r.bounds.x_min + 2);

        const int midX = r.midX();
        // 上内边距行、文本行、下内边距行中间单元格背景色均为 surfaceColor
        XX_TEST_EXPECT_EQ(r.bgAt(midX, r.bounds.y_min), fx.theme.surfaceColor);
        XX_TEST_EXPECT_EQ(r.bgAt(midX, r.bounds.y_min + 1), fx.theme.surfaceColor);
        XX_TEST_EXPECT_EQ(r.bgAt(midX, r.bounds.y_max), fx.theme.surfaceColor);

        // 文本前景色默认采用 style.title
        XX_TEST_EXPECT_EQ(r.screen.CellAt(tx, ty).foreground_color, style.title);

        // 左右各 2 格留白单元格为空格, 背景为 surfaceColor
        for (int dx = 0; dx <= 1; ++dx) {
            XX_TEST_EXPECT_EQ(r.screen.CellAt(r.bounds.x_min + dx, ty).character, " ");
            XX_TEST_EXPECT_EQ(r.bgAt(r.bounds.x_min + dx, ty), fx.theme.surfaceColor);
            XX_TEST_EXPECT_EQ(r.screen.CellAt(r.bounds.x_max - dx, ty).character, " ");
            XX_TEST_EXPECT_EQ(r.bgAt(r.bounds.x_max - dx, ty), fx.theme.surfaceColor);
        }

        // 上下内边距行除四角角标外均为纯空白填充
        for (int x = r.bounds.x_min + 1; x < r.bounds.x_max; ++x) {
            XX_TEST_EXPECT_TRUE(isBlankCell(r, x, r.bounds.y_min));
            XX_TEST_EXPECT_TRUE(isBlankCell(r, x, r.bounds.y_max));
        }
    }

    // 2. Light 主题 toast
    {
        SurfaceFixture fx;
        fx.theme         = TUITheme::lightTheme();
        const auto style = TuiSurfaceStyle::fromTheme(fx.theme);
        const auto toast = tuiSurfaceToast(style, "Light Toast");

        ProbeResult r(fx.kScreenW, fx.kScreenH);
        auto        el = toast | ftxui::center;
        ftxui::Render(r.screen, el);
        r.text = stripAnsi(r.screen.ToString());

        int xMin = fx.kScreenW, xMax = -1, yMin = fx.kScreenH, yMax = -1;
        for (int y = 0; y < fx.kScreenH; ++y) {
            for (int x = 0; x < fx.kScreenW; ++x) {
                if (r.bgAt(x, y) == ftxui::Color::Default) {
                    continue;
                }
                xMin = std::min(xMin, x);
                xMax = std::max(xMax, x);
                yMin = std::min(yMin, y);
                yMax = std::max(yMax, y);
            }
        }
        XX_TEST_EXPECT_TRUE(xMax >= 0);
        r.bounds = ftxui::Box{xMin, xMax, yMin, yMax};

        checkCornerCells(r, fx.theme, __LINE__);
        checkNoFrameGlyphs(r, __LINE__);
        XX_TEST_EXPECT_EQ(r.bounds.y_max - r.bounds.y_min + 1, 3);
        int tx = -1, ty = -1;
        XX_TEST_EXPECT_TRUE(r.findText("Light Toast", tx, ty));
        XX_TEST_EXPECT_EQ(tx, r.bounds.x_min + 2);
        XX_TEST_EXPECT_EQ(ty, r.bounds.y_min + 1);
        XX_TEST_EXPECT_EQ(r.screen.CellAt(tx, ty).foreground_color, style.title);
    }

    // 3. 多行换行 toast
    {
        SurfaceFixture fx;
        const auto     style = TuiSurfaceStyle::fromTheme(fx.theme);
        const auto toast = tuiSurfaceToast(style, "第一行提示\n第二行更长的提示文本");

        ProbeResult r(fx.kScreenW, fx.kScreenH);
        auto        el = toast | ftxui::center;
        ftxui::Render(r.screen, el);
        r.text = stripAnsi(r.screen.ToString());

        int xMin = fx.kScreenW, xMax = -1, yMin = fx.kScreenH, yMax = -1;
        for (int y = 0; y < fx.kScreenH; ++y) {
            for (int x = 0; x < fx.kScreenW; ++x) {
                if (r.bgAt(x, y) == ftxui::Color::Default) {
                    continue;
                }
                xMin = std::min(xMin, x);
                xMax = std::max(xMax, x);
                yMin = std::min(yMin, y);
                yMax = std::max(yMax, y);
            }
        }
        XX_TEST_EXPECT_TRUE(xMax >= 0);
        r.bounds = ftxui::Box{xMin, xMax, yMin, yMax};

        checkCornerCells(r, fx.theme, __LINE__);
        checkNoFrameGlyphs(r, __LINE__);
        // 2 行文本 + 上下各 1 行内边距 = 4 行
        XX_TEST_EXPECT_EQ(r.bounds.y_max - r.bounds.y_min + 1, 4);
    }

    TUISettings::instance().setLanguage(savedLang);

    return {g_tui_surface_passed, g_tui_surface_failed};
}

} // namespace test
} // namespace agentxx
